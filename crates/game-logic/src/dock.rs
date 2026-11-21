//! The server's docking and launching (work package P3.3, decision record
//! 0007 points 1 and 3): a ship's requests (`Ship::slot_5c`, `slot_60`,
//! `request_returned`, `Server/server_gameobject.cpp:5519-6075`), a dock
//! host's queue of requests (`EqObj::queue_dock_request`,
//! `dequeue_dock_request`, `process_dock_queue`, `:2480-2627`), the end of
//! a dock with a gate, a jump hole or an airlock (`StarSystem::
//! complete_dock`, `Server/server_classes.cpp:10184`), a player's launch
//! (`IServerImpl::RequestCreateShip` and `PlayerLaunch`,
//! `Server/IServerImpl.cpp:1386`, `1936`, with `Server_GetLaunchPosition`,
//! `server_classes.cpp:12070`), the client's request events and the
//! forced landing (`IServerImpl::RequestEvent`, `RequestCancel`, `Dock`).
//!
//! - **Requests.** A ship asks for a dock (0), a formation (1) or a trade
//!   lane (2). The server keeps the ship's busy flag (`Field_0x98`) and
//!   whether the request has a callback (the AI behaviour that made it,
//!   `PendingRequest`), and tells the bus (`DockRequest`,
//!   `FormationRequest`, `TradelaneRequest`). Content answers through
//!   `pub::SpaceObj::Dock` ([`crate::api::space_obj::dock`]): the answer
//!   goes to the behaviour (`World::request_returned`) or to the owner's
//!   client (`FLPACKET_SERVER_REQUEST_RETURNED`). The AI's requests reach
//!   the server after the behaviour tick that made them
//!   (`ServerHooks::server_requests`, D622).
//! - **The dock queue.** A granted dock puts `{port, ship}` on the host's
//!   queue and the host into the system's always-updated list (`ListD4`),
//!   once per entry. Each update of the host (`EqObj::update`) looks for
//!   each queued ship's `hpmount` inside the docking sphere around the
//!   port's launch point; a ship that is in docks: with a gate, a jump hole
//!   or an airlock (or a host without type bits) it completes the dock at
//!   once, with anything else (a station, a ring, a planet) it joins the
//!   system's landing queue ([`crate::world::queue_dock`]), landing unless
//!   the sphere is a mooring point. A ship that is gone leaves the queue.
//! - **`complete_dock`** starts the ship's death sequence
//!   ([`crate::ship`]): `DockComplete` to the bus, the dock as the
//!   sequence's object; a player's ship going through a gate or a jump
//!   hole becomes invincible, an NPC's waits for the client's jump-out in
//!   single player and loses its collisions on a multiplayer server, with
//!   12 s for a gate or a jump hole and none for anything else.
//! - **A player's launch** makes the player's ship in the system at the
//!   launch point of the base's first docking point (`launch_pos`), behind
//!   it by the ship's `HpMount`, from the slot's ship, equipment, collision
//!   groups and costume, and grants the launch invincibility when it
//!   launched from an object.
//!
//! An NPC's launch (`pub::SpaceObj::Launch`, `StarSystem::launch`) is in
//! [`crate::api::space_obj::launch`], over game-sim's `CShip::launch` and
//! its `LaunchAction`; its end lifts the launch invincibility
//! ([`crate::ship::launch_complete`]).
//!
//! Left to other parts: the behaviour events of a dock (event 5 to the
//! requesting ship's followers in `pub::SpaceObj::Dock`, event 3 to every
//! NPC ship's manager in `complete_dock`: the behaviour manager's server
//! link, `IBehaviorManager::Slot00`).

use game_sim::equip::types;
use game_sim::object::world::ServerRequest;
use game_sim::object::{ObjHandle, ObjectKind};
use neuron_core::math::{Matrix, Quaternion, Vector, matrix_to_quaternion, quaternion_to_matrix};
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::objects::Invincibility;
use crate::server::Cx;

const SERVER_SPACE: &str = "Server\\space\\ServerSpace.cpp";

/// `DOCK_HOST_RESPONSE` 3: the dock is busy; the ship waits (the request
/// stays pending).
pub const RESPONSE_WAIT: i32 = 3;
/// `DOCK_HOST_RESPONSE` 4: the dock is granted.
pub const RESPONSE_PROCEED: i32 = 4;

/// The type bits of a jump gate and a jump hole.
const GATE_BITS: u32 = 0x40 | 0x800;

/// One request on a dock host's queue (`FDockRequest`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DockRequest {
    /// `Port`: the docking sphere.
    pub port: i32,
    /// `ShipId`.
    pub ship: u32,
}

/// `Server_LookupLiveObject`: the object of an id, unless it is destroyed.
#[must_use]
pub fn lookup_live(cx: &Cx<'_>, id: u32) -> Option<ObjHandle> {
    let h = cx.world.find(id)?;
    let o = cx.world.get(h)?;
    if o.dead || cx.logic.objects.get(h).is_some_and(|s| s.destroyed) {
        return None;
    }
    Some(h)
}

fn is_ship(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.kind == ObjectKind::Ship && o.equipped.is_some())
}

fn id_and_system(cx: &Cx<'_>, h: ObjHandle) -> (u32, u32) {
    cx.world
        .get(h)
        .map_or((0, 0), |o| (o.simple.id, o.physical.system))
}

fn type_bits(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).map_or(0, |o| o.simple.type_bits)
}

// ---- a ship's requests -----------------------------------------------------------------------

/// `Ship::slot_5c(Kind, &Target, &Second, Flag, Callback)`: refused (0)
/// while the ship has a request; otherwise the request becomes the ship's,
/// with or without a callback, and goes to the bus: `DockRequest` (type 0,
/// with `Flag == 1`), `FormationRequest` (1), `TradelaneRequest` (2, with
/// the far ring). Any other type only takes the callback. Answers 0, or -3
/// for an object that is not a ship (`GameObject::slot_5c`).
pub fn request(
    cx: &mut Cx<'_>,
    ship: ObjHandle,
    kind: i32,
    target: u32,
    second: u32,
    flag: i32,
    callback: bool,
) -> i32 {
    if !is_ship(cx, ship) {
        return -3;
    }
    let Some(s) = crate::ship::ship_state(cx, ship) else {
        return -3;
    };
    if s.request_busy {
        return 0;
    }
    s.request_callback = callback;
    s.request_busy = true;
    let (id, system) = id_and_system(cx, ship);
    let mut message = match kind {
        0 => ControllerMessage::DockRequest {
            ship: id,
            target,
            system,
            flag: flag == 1,
        },
        1 => ControllerMessage::FormationRequest {
            ship: id,
            leader: target,
            system,
        },
        2 => ControllerMessage::TradelaneRequest {
            ship: id,
            ring: target,
            end_ring: second,
            system,
        },
        _ => {
            if let Some(s) = crate::ship::ship_state(cx, ship) {
                s.request_busy = false;
            }
            return 0;
        }
    };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
    0
}

/// `Ship::slot_60(Kind, &Target, Arg3)`: the request and its callback are
/// dropped, the bus hears `DockCancel` (type 0), `FormationLeave` (1) or
/// `TradelaneRelease` (2), and on every path the invincibility of type 2
/// ends. Answers 0, or -3 for an object that is not a ship.
pub fn cancel_request(cx: &mut Cx<'_>, ship: ObjHandle, kind: i32, target: u32, arg: i32) -> i32 {
    if !is_ship(cx, ship) {
        return -3;
    }
    if let Some(s) = crate::ship::ship_state(cx, ship) {
        s.request_callback = false;
        s.request_busy = false;
    }
    let (id, system) = id_and_system(cx, ship);
    let message = match kind {
        0 => Some(ControllerMessage::DockCancel {
            ship: id,
            target,
            arg: arg as u32,
        }),
        1 => Some(ControllerMessage::FormationLeave {
            ship: id,
            leader: target,
            system,
        }),
        2 => Some(ControllerMessage::TradelaneRelease {
            ship: id,
            ring: target,
            system,
        }),
        _ => None,
    };
    if let Some(mut message) = message {
        let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
    }
    if let Some(o) = cx.logic.objects.entry(cx.world, ship) {
        o.set_invincible(Invincibility {
            kind: 2,
            protect: false,
            from_players: false,
            max_hp_loss: 1.0,
        });
    }
    0
}

/// `Ship::request_returned(Kind, Response, Arg3)`: with a callback, the
/// answer goes to the requesting behaviour's `OnResult`, and a request it
/// does not keep ends (callback and busy flag cleared); without one, the
/// owner's client hears it (`SendServerRequestReturned`) and the ship stays
/// busy only while a dock request waits (type 0, response 3).
pub fn request_returned(cx: &mut Cx<'_>, ship: ObjHandle, kind: i32, response: i32, arg: i32) {
    let Some(s) = crate::ship::ship_state(cx, ship) else {
        return;
    };
    if s.request_callback {
        let kept = cx
            .world
            .request_returned(ship, kind, response, arg)
            .unwrap_or(false);
        if !kept && let Some(s) = crate::ship::ship_state(cx, ship) {
            s.request_callback = false;
            s.request_busy = false;
        }
        return;
    }
    let (owner, id) = cx
        .world
        .get(ship)
        .map_or((0, 0), |o| (o.simple.owner_player, o.simple.id));
    crate::client::request_returned(cx, owner, id, kind, response, arg);
    if let Some(s) = crate::ship::ship_state(cx, ship) {
        s.request_busy = kind == 0 && response == RESPONSE_WAIT;
    }
}

/// The AI's requests of a behaviour tick (`ServerHooks::server_requests`):
/// each a `slot_5c` with the behaviour as its callback, a `slot_60`, or
/// the server's part of a shot ([`crate::equip_fire::ai_fired`]).
pub fn server_requests(cx: &mut Cx<'_>, requests: Vec<ServerRequest>) {
    for r in requests {
        match r {
            ServerRequest::Request {
                ship,
                kind,
                target,
                second,
                flag,
            } => {
                request(cx, ship, kind, target, second, flag, true);
            }
            ServerRequest::Cancel {
                ship,
                kind,
                target,
                arg,
            } => {
                cancel_request(cx, ship, kind, target, arg);
            }
            ServerRequest::Fired {
                ship,
                aim,
                results,
                guided,
            } => crate::equip_fire::ai_fired(cx, ship, aim, &results, &guided),
        }
    }
}

/// `IServerImpl::RequestEvent(Kind, Ship, Target, Second, Flag, _)`: a
/// client's request (`slot_5c` without a callback) of an object the
/// server knows.
pub fn request_event(cx: &mut Cx<'_>, kind: i32, ship: u32, target: u32, second: u32, flag: u32) {
    if let Some(h) = cx.world.find(ship) {
        request(cx, h, kind, target, second, flag as i32, false);
    }
}

/// `IServerImpl::RequestCancel(Kind, Ship, Target, Arg, _)`: a client's
/// cancel (`slot_60`).
pub fn request_cancel(cx: &mut Cx<'_>, kind: i32, ship: u32, target: u32, arg: u32) {
    if let Some(h) = cx.world.find(ship) {
        cancel_request(cx, h, kind, target, arg as i32);
    }
}

// ---- the dock host's queue -------------------------------------------------------------------

/// `StarSystem::add_to_list_d4`: an object with any type bit but the
/// waypoint's gets one more always-updated entry.
fn add_to_list_d4(cx: &mut Cx<'_>, host: ObjHandle) {
    if type_bits(cx, host) & 0xfdff != 0 {
        cx.world.add_always_update(host);
    }
}

/// `EqObj::queue_dock_request(Port, Ship)`: the request at the queue's end
/// and the host one more always-updated entry.
pub fn queue_dock_request(cx: &mut Cx<'_>, host: ObjHandle, port: i32, ship: u32) {
    let Some(o) = cx.logic.objects.entry(cx.world, host) else {
        return;
    };
    o.dock_queue.push(DockRequest { port, ship });
    add_to_list_d4(cx, host);
}

/// `EqObj::dequeue_dock_request(Port, Ship)`: the ship's first request (at
/// that port, or at any for a negative one) leaves the queue and the host
/// one always-updated entry. False when there is none.
pub fn dequeue_dock_request(cx: &mut Cx<'_>, host: ObjHandle, port: i32, ship: u32) -> bool {
    let Some(o) = cx.logic.objects.entry_mut(host) else {
        return false;
    };
    let Some(at) = o
        .dock_queue
        .iter()
        .position(|r| r.ship == ship && (port < 0 || r.port == port))
    else {
        return false;
    };
    o.dock_queue.remove(at);
    cx.world.remove_always_update(host);
    true
}

/// Removes a request the walk handled: at its place, or, when a handler
/// changed the queue meanwhile, the first equal one (D627).
fn erase_request(cx: &mut Cx<'_>, host: ObjHandle, at: usize, request: DockRequest) {
    if let Some(o) = cx.logic.objects.entry_mut(host) {
        if o.dock_queue.get(at) == Some(&request) {
            o.dock_queue.remove(at);
        } else if let Some(i) = o.dock_queue.iter().position(|r| *r == request) {
            o.dock_queue.remove(i);
        }
    }
    cx.world.remove_always_update(host);
}

/// `EqObj::destroy`'s and `destroy_notify`'s release: one always-updated
/// entry less for each queued request (the queue itself stays).
pub fn release_always_update(cx: &mut Cx<'_>, host: ObjHandle) {
    let count = cx.logic.objects.get(host).map_or(0, |o| o.dock_queue.len());
    for _ in 0..count {
        cx.world.remove_always_update(host);
    }
}

/// `EqObj::process_dock_queue`: each queued request in order (those queued
/// during the walk too). A ship the host's system does not have leaves the
/// queue. Otherwise the ship's `hpmount` in the world (`FindHardpoint_OS`
/// through the root's transform) is measured against the port's launch
/// point (`launch_pos`); unless it lies inside the port's docking sphere
/// (the squared radius above the squared distance, in doubles) the ship
/// waits (a destroyed ship has left the system's ship list by then, as its
/// wrapper's update does before the near-ship pass). A ship inside docks: a host whose type bits are a subset of
/// `0x440` (a gate, an airlock, none) or that has `0x800` (a jump hole)
/// completes the dock ([`complete_dock`]); any other joins the system's
/// landing queue with the host's docking-point base, landing unless the
/// sphere is a mooring point (types 3, 4 and 5). The request then leaves
/// the queue and the host one always-updated entry. A port without a
/// docking sphere never takes the ship in (D623).
pub fn process_dock_queue(cx: &mut Cx<'_>, host: ObjHandle) {
    let mut at = 0;
    loop {
        let Some(req) = cx
            .logic
            .objects
            .get(host)
            .and_then(|o| o.dock_queue.get(at).copied())
        else {
            return;
        };
        let system = cx.world.get(host).map_or(0, |o| o.physical.system);
        let ship = cx.world.find(req.ship).filter(|&h| {
            cx.world.get(h).is_some_and(|o| {
                o.kind == ObjectKind::Ship && !o.dead && o.simple.system == Some(system)
            })
        });
        let Some(ship) = ship else {
            erase_request(cx, host, at, req);
            continue;
        };
        let sphere = cx.world.get(host).and_then(|o| {
            let eq = o.as_equipped()?;
            let i = usize::try_from(req.port).ok()?;
            eq.docking_points.get(i).map(|d| (d.radius, d.dock_type))
        });
        let (_, launch, _) = cx.world.launch_pos(host, req.port);
        let mount = cx
            .world
            .hardpoint_os_world(ship, b"hpmount")
            .or_else(|| cx.world.root_translation(ship))
            .unwrap_or(Vector::ZERO);
        let d = |a: f32, b: f32| f64::from((f64::from(a) - f64::from(b)) as f32);
        let (dx, dy, dz) = (
            d(mount.x, launch.x),
            d(mount.y, launch.y),
            d(mount.z, launch.z),
        );
        let dist_sq = (dy * dy + dz * dz) + dx * dx;
        let (radius, dock_type) = sphere.unwrap_or((0.0, 0));
        // `TEST AH,0x41 / JNZ`: equal and unordered keep the ship queued.
        let inside = f64::from(radius) * f64::from(radius) > dist_sq;
        if !inside {
            at += 1;
            continue;
        }
        let bits = type_bits(cx, host);
        if bits == bits & 0x440 || bits & 0x800 != 0 {
            complete_dock(cx, system, ship, host);
        } else {
            let land = !matches!(dock_type, 3..=5);
            let base = cx
                .world
                .get(host)
                .and_then(|o| o.as_equipped())
                .map_or(0, |e| e.docking_point_base);
            crate::world::queue_dock(cx, system, Some(ship), Some(host), base, land);
        }
        erase_request(cx, host, at, req);
    }
}

/// `StarSystem::complete_dock(Ship, Dock)` in the dock's system: nothing
/// for a ship already in a death sequence; otherwise `DockComplete` (ship,
/// dock, system), every player's `OnObjectLeftSystem`
/// ([`crate::replication::ship_left_for_dock`]) and the dock as the
/// sequence's object. A player's ship
/// that went through a gate or a jump hole gets the invincibility of type 2
/// against everyone with no loss allowed; an NPC's ship in single player
/// waits for its jump-out again; on a multiplayer server an NPC's ship
/// loses its collisions and its sequence lasts 12 s for a gate or a jump
/// hole and none otherwise. An object that is not a ship, which the C++
/// casts unchecked, does nothing (D625).
pub fn complete_dock(cx: &mut Cx<'_>, system: u32, ship: ObjHandle, dock: ObjHandle) {
    match crate::ship::ship_state(cx, ship) {
        Some(s) if s.death_sequence_obj_id == 0 => {}
        _ => return,
    }
    // The behaviour event 3 to every NPC ship's manager comes with the
    // behaviour manager's server link.
    let ship_id = cx.world.get(ship).map_or(0, |o| o.simple.id);
    let dock_id = cx.world.get(dock).map_or(0, |o| o.simple.id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::DockComplete {
            ship: ship_id,
            dock: dock_id,
            system,
        },
    );
    // Every player hears that the ship left for the dock.
    crate::replication::ship_left_for_dock(cx, system, ship, dock);
    let dock_id = cx.world.get(dock).map_or(dock_id, |o| o.simple.id);
    if let Some(s) = crate::ship::ship_state(cx, ship) {
        s.death_sequence_obj_id = dock_id;
    }
    let owner = cx.world.get(ship).map_or(0, |o| o.simple.owner_player);
    let gate = type_bits(cx, dock) & GATE_BITS != 0;
    if owner != 0 {
        if gate && let Some(o) = cx.logic.objects.entry(cx.world, ship) {
            o.set_invincible(Invincibility {
                kind: 2,
                protect: true,
                from_players: true,
                max_hp_loss: 0.0,
            });
        }
        return;
    }
    if cx.world.config().single_player {
        if let Some(s) = cx.world.get_mut(ship).and_then(|o| o.as_ship_mut()) {
            s.jumped_out = false;
        }
        return;
    }
    cx.world.set_collisions(ship, false);
    if let Some(s) = crate::ship::ship_state(cx, ship) {
        s.death_sequence_timer = if gate { 12.0 } else { 0.0 };
    }
}

/// `IServerImpl::Dock(Base, Player)`, the forced landing: the player's ship
/// joins its system's landing queue for the base (no target, landing).
/// When the base lies in another system, that system's frame runs at once
/// (0.2 s), the visits are flushed, `SystemExit` and `SystemEnter` (with no
/// arrival) go to the bus, and the slot's system becomes the base's. A
/// ship in transit has no system; the C++ reads through the null (D626).
pub fn force_land(cx: &mut Cx<'_>, base: u32, session: u32) {
    let Some(ship_id) = cx.logic.players.get(session).map(|p| p.ship_object_id) else {
        return;
    };
    let Some(ship) = cx.world.find(ship_id) else {
        return;
    };
    let Some(system) = cx.world.get(ship).and_then(|o| o.simple.system) else {
        return;
    };
    let Some(base_system) = cx.world.data().universe.bases.get(&base).map(|b| b.system) else {
        return;
    };
    crate::world::queue_dock(cx, system, Some(ship), None, base, true);
    if cx
        .logic
        .players
        .get(session)
        .is_some_and(|p| p.system_id == base_system)
    {
        return;
    }
    crate::world::step_system(cx, system, 0.2);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        crate::player::flush_pending_visits(slot);
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SystemExit {
            system,
            destination: base_system,
            player: session,
        },
    );
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.system_id = base_system;
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SystemEnter {
            system: base_system,
            from: system,
            player: session,
            arrival: 0,
            position: Vector::ZERO,
        },
    );
}

// ---- a player's launch -----------------------------------------------------------------------

/// Where a player's ship launches (`Server_GetLaunchPosition`'s outputs).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LaunchPosition {
    /// The object launched from (0 none).
    pub object: u32,
    /// The launch point's index (-1 none).
    pub kind: i32,
    /// The launch point.
    pub position: Vector,
    /// The orientation.
    pub orientation: Quaternion,
}

/// `IServerImpl::RequestCreateShip`: a valid session's reserved ship id,
/// made once (a new object id) and answered to the client
/// (`FLPACKET_SERVER_REQUESTCREATESHIPRESP`).
pub fn request_create_ship(cx: &mut Cx<'_>, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let reserved = cx
        .logic
        .players
        .get(session)
        .map_or(0, |p| p.created_ship_id);
    if reserved & 0x3fff_ffff != 0 {
        crate::client::request_create_ship_resp(cx, session, true, reserved);
        return;
    }
    let id = cx.world.allocate_id();
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.created_ship_id = id;
    }
    crate::client::request_create_ship_resp(cx, session, id != 0, id);
}

/// `StarSystem::collect_dockable_solars(Out, Base)`: the system's solars
/// with a docking-point base (that base, unless `base` is 0), and with
/// `base` 0 those whose type bits are a subset of a gate's, a jump hole's
/// and an airlock's.
#[must_use]
pub fn collect_dockable_solars(cx: &Cx<'_>, system: u32, base: u32) -> Vec<ObjHandle> {
    cx.world
        .objects_in(system)
        .into_iter()
        .filter(|&h| {
            let Some(o) = cx.world.get(h).filter(|o| o.kind == ObjectKind::Solar) else {
                return false;
            };
            let Some(eq) = o.as_equipped() else {
                return false;
            };
            if eq.docking_point_base != 0 {
                return base == 0 || eq.docking_point_base == base;
            }
            let bits = o.simple.type_bits;
            bits & 0xc40 == bits && base == 0
        })
        .collect()
}

/// `StarSystem::find_solar(Nickname)`: the first solar of the system with
/// that nickname.
#[must_use]
pub fn find_solar(cx: &Cx<'_>, system: u32, nickname: u32) -> Option<ObjHandle> {
    cx.world.objects_in(system).into_iter().find(|&h| {
        cx.world.get(h).is_some_and(|o| match &o.data {
            game_sim::object::KindData::Solar(s) => s.nickname == nickname,
            _ => false,
        })
    })
}

fn launch_from(cx: &Cx<'_>, ring: ObjHandle, object: u32, kind: i32) -> LaunchPosition {
    let (_, position, orient) = cx.world.launch_pos(ring, kind);
    LaunchPosition {
        object,
        kind,
        position,
        orientation: matrix_to_quaternion(&orient),
    }
}

fn initial_position(cx: &Cx<'_>, session: u32) -> LaunchPosition {
    let (pos, orient) = cx.logic.players.get(session).map_or(
        (
            [0.0; 3],
            [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
        ),
        |p| (p.initial_pos, p.initial_orient),
    );
    LaunchPosition {
        object: 0,
        kind: -1,
        position: Vector::new(pos[0], pos[1], pos[2]),
        orientation: matrix_to_quaternion(&Matrix::from_rows(orient)),
    }
}

/// `Server_GetLaunchPosition`. A player who left a base launches from the
/// first dockable solar of the base in the slot's system: a docking ring
/// from its point 1, anything else from the terminal SpaceSupport reports
/// (`FreeTerminalRequest`, 0 when none is reported); the base left is
/// forgotten. With no such solar (reported) the ship starts 2.2 physical
/// radii from the base's object on the side of the system's centre,
/// looking at it. A player who left no base launches from the
/// `starting_ring` (point 1 of a docking ring, else 0), or at the slot's
/// initial position. Where the C++ reads a missing system, base or ring
/// through null, the port takes the initial position (D626).
pub fn launch_position(cx: &mut Cx<'_>, session: u32) -> LaunchPosition {
    let Some(slot) = cx.logic.players.get(session) else {
        return initial_position(cx, session);
    };
    let (system, base, ring) = (slot.system_id, slot.exited_base_id, slot.starting_ring);
    if base & 0x3fff_ffff != 0 {
        let rings = collect_dockable_solars(cx, system, base);
        let out = if let Some(&first) = rings.first() {
            let object = cx.world.get(first).map_or(0, |o| o.simple.id);
            let kind = if type_bits(cx, first) & 0x20 != 0 {
                1
            } else {
                let mut message = ControllerMessage::FreeTerminalRequest {
                    player: session,
                    target: object,
                    reported: -1,
                };
                let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
                match message {
                    ControllerMessage::FreeTerminalRequest { reported, .. } if reported != -1 => {
                        reported
                    }
                    _ => 0,
                }
            };
            match cx.world.get(first) {
                Some(_) => launch_from(cx, first, object, kind),
                None => initial_position(cx, session),
            }
        } else {
            cx.logic.reports.push(
                SERVER_SPACE,
                0x1a3,
                format!("*** WARNING: No docking rings for base {base:X}"),
            );
            let found = cx.world.find(base).filter(|&h| {
                cx.world
                    .get(h)
                    .is_some_and(|o| o.simple.system == Some(system))
            });
            match found {
                Some(h) => base_side_position(cx, h),
                None => initial_position(cx, session),
            }
        };
        if let Some(slot) = cx.logic.players.get_mut(session) {
            slot.exited_base_id = 0;
        }
        return out;
    }
    if ring != 0 {
        if let Some(h) = find_solar(cx, system, ring) {
            let object = cx.world.get(h).map_or(0, |o| o.simple.id);
            let kind = i32::from(type_bits(cx, h) & 0x20 != 0);
            return launch_from(cx, h, object, kind);
        }
        return initial_position(cx, session);
    }
    initial_position(cx, session)
}

/// The no-ring fallback: 2.2 physical radii out from the base's object
/// toward the system's centre, looking there (`LookMatrix`).
fn base_side_position(cx: &Cx<'_>, base: ObjHandle) -> LaunchPosition {
    let p = cx
        .world
        .get(base)
        .map_or(Vector::ZERO, |o| o.engine.position);
    let dir = Vector::new(-p.x, -p.y, -p.z);
    let len = (f64::from(dir.x) * f64::from(dir.x)
        + f64::from(dir.y) * f64::from(dir.y)
        + f64::from(dir.z) * f64::from(dir.z))
    .sqrt();
    let inv = 1.0 / len;
    let dir = Vector::new(
        (f64::from(dir.x) * inv) as f32,
        (f64::from(dir.y) * inv) as f32,
        (f64::from(dir.z) * inv) as f32,
    );
    let (radius, _) = cx.world.physical_radius_r(base);
    let scale = |a: f32, b: f32| (f64::from(a) * f64::from(b)) as f32;
    let scaled = Vector::new(
        scale(radius, dir.x),
        scale(radius, dir.y),
        scale(radius, dir.z),
    );
    let off = Vector::new(
        scale(2.2, scaled.x),
        scale(2.2, scaled.y),
        scale(2.2, scaled.z),
    );
    let add = |a: f32, b: f32| (f64::from(a) + f64::from(b)) as f32;
    LaunchPosition {
        object: 0,
        kind: -1,
        position: Vector::new(add(p.x, off.x), add(p.y, off.y), add(p.z, off.z)),
        orientation: matrix_to_quaternion(&game_sim::object::csys::look_matrix(dir)),
    }
}

/// `IServerImpl::PlayerLaunch(Ship, Player)`: for a valid session not in a
/// ship whose reserved ship is `ship_id`, in a system the server has. The
/// player joins the system (`add_player`), `PlayerLaunch` goes to the bus
/// with the base left; the launch point ([`launch_position`]) less the
/// archetype's `HpMount` turned by the launch orientation is the ship's
/// position, which the client hears (`FLPACKET_SERVER_LAUNCH`, and on a
/// multiplayer server the ship's group and affiliation). The ship is made
/// ([`crate::ship::create_ship_with_groups`]) from the slot: archetype,
/// equipment, collision groups, comm costume, voice, reputation group and
/// its rank, its relative health of the archetype's hit points (truncated),
/// owned by the player, at rest. A launch from an object grants the launch
/// invincibility; in single player the client's ship is beamed to the
/// point. The slot's ship is set and `SpaceEnter` goes to the bus. An
/// unknown ship archetype, which the C++ loads through null, launches no
/// ship (D626).
pub fn player_launch(cx: &mut Cx<'_>, ship_id: u32, session: u32) {
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    if !cx.logic.players.is_valid(session)
        || slot.ship_object_id & 0x3fff_ffff != 0
        || slot.created_ship_id & 0x3fff_ffff == 0
        || ship_id != slot.created_ship_id
    {
        return;
    }
    let system = slot.system_id;
    crate::world::initialize(cx);
    if system & 0x3fff_ffff == 0 || !cx.logic.server_world.systems.contains_key(&system) {
        return;
    }
    crate::world::add_player(cx, system, session);
    let exited = cx
        .logic
        .players
        .get(session)
        .map_or(0, |p| p.exited_base_id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::PlayerLaunch {
            player: session,
            base: exited,
        },
    );
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    let archetype = slot.ship_archetype;
    let group = slot.group.unwrap_or(0);
    let Some(arch_hit_pts) = cx
        .world
        .data()
        .archetypes
        .ships
        .get(&archetype)
        .map(|a| a.eq_obj.root.hit_pts)
    else {
        return;
    };
    let hp_pos = cx
        .world
        .ship_archetype_hardpoint(archetype, b"HpMount")
        .map_or(Vector::ZERO, |t| t.translation);
    let launch = launch_position(cx, session);
    let orient = quaternion_to_matrix(&launch.orientation);
    let offset = neuron_core::math::transform_matrix(&orient, hp_pos);
    let sub = |a: f32, b: f32| (f64::from(a) - f64::from(b)) as f32;
    let position = Vector::new(
        sub(launch.position.x, offset.x),
        sub(launch.position.y, offset.y),
        sub(launch.position.z, offset.z),
    );
    crate::client::launch(
        cx,
        session,
        ship_id,
        launch.object,
        launch.kind,
        position,
        launch.orientation,
    );
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    let vibe = slot.rep_vibe;
    if !cx.world.config().single_player {
        crate::client::misc_obj_update_object_group(cx, session, ship_id, group);
        let (_, affiliation) = cx.world.vibes().0.affiliation(vibe, false);
        crate::client::misc_obj_update_affiliation(cx, session, ship_id, affiliation);
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    let hit_pts =
        neuron_core::num::trunc_to_i32(f64::from(slot.relative_health) * f64::from(arch_hit_pts));
    let voice = neuron_core::id::create_id(&slot.voice);
    let request = crate::ship::ShipCreateRequest {
        id: ship_id,
        archetype,
        loadout: 0,
        position,
        orientation: launch.orientation,
        velocity: Vector::ZERO,
        angular_velocity: Vector::ZERO,
        owner_player: session,
        hit_pts,
        equipment: slot.equipment.clone(),
        vibe,
        costume: slot.com_costume.clone(),
        voice,
        docking_base: 0,
        group_name_id: 0,
        rank: 0,
        using_tradelane: false,
        mark_player: 0,
        equip_ids: Default::default(),
        equip_state_data: 0,
    };
    let groups = slot.collision_groups.clone();
    let request = crate::ship::ShipCreateRequest {
        rank: cx.world.vibes().0.rank(vibe).1,
        ..request
    };
    let made = crate::ship::create_ship_with_groups(cx, system, &request, Some(&groups));
    if let Some(h) = made {
        if launch.object != 0 {
            crate::ship::apply_launch_invincibility(cx, h);
        }
        if cx.world.config().single_player {
            cx.world.beam_object(h, position, orient);
        }
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.ship_object_id = ship_id;
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SpaceEnter {
            system,
            player: session,
            launch_object: launch.object,
            launch_kind: launch.kind,
        },
    );
}

/// `pub::SpaceObj::ActivateEquipment` with `EQInfo` type 3: every docking
/// light of an equipped object switched on or off (`EqObj::slot_4c` from
/// the server: a destroyed object or item is left alone, and each switch
/// tells the players who can see the object,
/// [`crate::replication::equip_activated`]). The other
/// `EQInfo` types come with their parts.
pub fn activate_docking_lights(cx: &mut Cx<'_>, h: ObjHandle, on: bool) {
    if cx.logic.objects.get(h).is_some_and(|o| o.destroyed) {
        return;
    }
    let lights: Vec<(u16, bool)> = {
        let data = cx.world.data();
        cx.world
            .get(h)
            .and_then(|o| o.as_equipped())
            .map(|eq| {
                eq.equipment
                    .traverse(types::LIGHT, false)
                    .into_iter()
                    .filter_map(|slot| eq.equipment.get(slot))
                    .filter(|e| {
                        matches!(
                            data.archetypes.equipment.get(&e.arch_id).map(|a| &a.kind),
                            Some(game_sim::data::archetype::equipment::EquipmentKind::Light {
                                docking_light: true,
                                ..
                            })
                        )
                    })
                    .map(|e| (e.id, e.is_destroyed() || e.is_active() == on))
                    .collect()
            })
            .unwrap_or_default()
    };
    let (system, object) = cx
        .world
        .get(h)
        .map_or((0, 0), |o| (o.physical.system, o.simple.id));
    for (id, skip) in lights {
        if !skip {
            cx.world
                .command(h, game_sim::object::Command::ActivateEquip(id, on));
            crate::replication::equip_activated(cx, system, object, id, on, 0);
        }
    }
}
