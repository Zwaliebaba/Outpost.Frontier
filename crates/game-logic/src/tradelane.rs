//! The trade lanes on the server (work package P3.3, decision record 0007):
//! the server's `Ship::go_tradelane` and `stop_tradelane`
//! (`Server/server_gameobject.cpp:6076-6137`) around game-sim's
//! `LaneAction` (`object::world::lane_action`), and SpaceSupport's answer
//! to a ship's lane request (`Content/TradelaneRequests.cpp`, TU55, which
//! `Content/SpaceSupport.cpp` calls for the messages 0x14 and 0x15).
//!
//! - **The request.** An NPC's dock behaviour asks for the lane at the
//!   ring (`Ship::slot_5c` type 2, [`crate::dock::request`]), which the
//!   bus hears as `TradelaneRequest`. SpaceSupport answers
//!   ([`handle_request`]): an object that is not a lane ring (type bit
//!   0x80) gets response 5; a ring grants it with response 4
//!   (`pub::SpaceObj::LaneResponse`, which reaches the requesting
//!   behaviour through `Ship::request_returned`) and is activated for the
//!   ship (`pub::SpaceObj::Activate(Ring, true, Ship)`). The cancel
//!   (`TradelaneRelease`) deactivates it ([`handle_release`]).
//! - **The hand-off.** The behaviour, at the ring's mount, sends the ship
//!   into the lane (`IObjAffect::go_tradelane`): the world makes the
//!   action at once and the server's part follows the behaviour tick as a
//!   [`ServerRequest::GoTradeLane`] ([`ai_go_tradelane`], D730), which
//!   tells the players who see the ship, the bus and the other NPC ships'
//!   behaviours ([`crate::replication::notify_go_tradelane`]).
//! - **The end.** The action, at its end, calls `stop_tradelane` on the
//!   ship ([`ServerRequest::StopTradeLane`] after the physics step, D731):
//!   the players, the bus and the NPC ships' behaviours hear it
//!   ([`crate::replication::stop_tradelane`]), the ship's own instant
//!   trade-lane behaviour among them, which ends its directive and
//!   cancels the request.
//!
//! [`go_tradelane`] and [`stop_tradelane`] are the server's own calls of
//! the two `Ship` functions on a ship it knows.

use game_sim::object::world::ServerRequest;
use game_sim::object::{ObjHandle, ObjectKind};

use crate::events::ControllerMessage;
use crate::server::Cx;

/// `DOCK_HOST_RESPONSE` 4: the lane is granted.
pub const LANE_GRANTED: i32 = 4;

/// `DOCK_HOST_RESPONSE` 5: refused (not a lane ring).
pub const LANE_REFUSED: i32 = 5;

/// The ship's system (`OwnerSystem`), none for an object in transit.
fn owner_system(cx: &Cx<'_>, h: ObjHandle) -> Option<u32> {
    cx.world.get(h).and_then(|o| o.simple.system)
}

fn is_ship(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Ship)
}

fn id_of(cx: &Cx<'_>, h: Option<ObjHandle>) -> u32 {
    h.and_then(|h| cx.world.get(h)).map_or(0, |o| o.simple.id)
}

/// `Ship::go_tradelane(Entrance, Exit, bFastExit, Speed)`: `CShip::
/// go_tradelane` with the ship as the traveller (-5 when it fails), then
/// the ship's system's `notify_go_tradelane` with the rings' ids (0 for no
/// exit ring); 0. -3 for an object that is not a ship
/// (`GameObject::go_tradelane`).
pub fn go_tradelane(
    cx: &mut Cx<'_>,
    ship: ObjHandle,
    entrance: Option<ObjHandle>,
    exit: Option<ObjHandle>,
    fast_exit: bool,
    speed: f32,
) -> i32 {
    if !is_ship(cx, ship) {
        return -3;
    }
    if cx
        .world
        .go_tradelane(ship, entrance, exit, fast_exit, speed)
        != 0
    {
        return -5;
    }
    let entrance = id_of(cx, entrance);
    let exit = id_of(cx, exit);
    notify_go(cx, ship, entrance, exit, fast_exit, speed);
    0
}

/// `Ship::stop_tradelane`: `CShip::stop_tradelane`, then the rings the
/// action reports (`get_tradelane_exit_ring`, which reads the current
/// leg's first ring, and `get_tradelane_entrance_ring`) to the ship's
/// system's `notify_stop_tradelane`; 0. -3 for an object that is not a
/// ship.
pub fn stop_tradelane(cx: &mut Cx<'_>, ship: ObjHandle) -> i32 {
    if !is_ship(cx, ship) {
        return -3;
    }
    cx.world.stop_tradelane(ship);
    let exit = cx.world.tradelane_exit_ring(ship);
    let entrance = cx.world.tradelane_entrance_ring(ship);
    notify_stop(cx, ship, entrance, exit);
    0
}

/// The server's part of the AI's `go_tradelane`
/// ([`ServerRequest::GoTradeLane`]): the world made the action.
pub fn ai_go_tradelane(
    cx: &mut Cx<'_>,
    ship: ObjHandle,
    entrance: u32,
    exit: u32,
    fast_exit: bool,
    speed: f32,
) {
    notify_go(cx, ship, entrance, exit, fast_exit, speed);
}

/// The server's part of `stop_tradelane` on a ship the world stopped
/// ([`ServerRequest::StopTradeLane`]).
pub fn ai_stop_tradelane(cx: &mut Cx<'_>, ship: ObjHandle, entrance: u32, exit: u32) {
    notify_stop(cx, ship, entrance, exit);
}

fn notify_go(
    cx: &mut Cx<'_>,
    ship: ObjHandle,
    entrance: u32,
    exit: u32,
    fast_exit: bool,
    speed: f32,
) {
    let Some(system) = owner_system(cx, ship) else {
        return;
    };
    let id = id_of(cx, Some(ship));
    crate::replication::notify_go_tradelane(cx, system, id, entrance, exit, fast_exit, speed);
}

fn notify_stop(cx: &mut Cx<'_>, ship: ObjHandle, entrance: u32, exit: u32) {
    let Some(system) = owner_system(cx, ship) else {
        return;
    };
    let id = id_of(cx, Some(ship));
    crate::replication::stop_tradelane(cx, system, id, entrance, exit);
}

/// `HandleTradelaneRequest` (SpaceSupport's message 0x14): response 5
/// unless the object asked for is a lane ring (`pub::SpaceObj::GetType`'s
/// bit 0x80); a ring grants the lane (4). The answer goes to the ship
/// (`pub::SpaceObj::LaneResponse`), and a granted ring is activated for
/// it (`pub::SpaceObj::Activate(Ring, true, Ship)`). A player's route
/// check (`FContentPlayerState::CheckTradelaneRoute`) and the comm system's
/// "granted" conversation are Content's, not ported (D734); an unknown
/// ring, whose type the C++ reads from a stale stack slot, reads 0 (D735).
pub fn handle_request(cx: &mut Cx<'_>, ship: u32, ring: u32, _end_ring: u32, _system: u32) {
    let ring_type = crate::api::space_obj::object_type(cx, ring).unwrap_or(0);
    let response = if ring_type & 0x80 != 0 {
        LANE_GRANTED
    } else {
        LANE_REFUSED
    };
    let _ = crate::api::space_obj::lane_response(cx, ship, response);
    if response == LANE_GRANTED {
        let _ =
            crate::api::space_obj::activate(cx, ring, true, i32::from_ne_bytes(ship.to_ne_bytes()));
    }
}

/// `HandleTradelaneRelease` (SpaceSupport's message 0x15): the ring
/// deactivated for the ship (`pub::SpaceObj::Activate(Ring, false, Ship)`).
pub fn handle_release(cx: &mut Cx<'_>, ship: u32, ring: u32) {
    let _ =
        crate::api::space_obj::activate(cx, ring, false, i32::from_ne_bytes(ship.to_ne_bytes()));
}

/// SpaceSupport's trade-lane messages (`Content/SpaceSupport.cpp:669`),
/// the part of the controller this module ports: `TradelaneRequest` and
/// `TradelaneRelease` to their handlers; any other message is not this
/// module's.
pub fn space_support(cx: &mut Cx<'_>, message: &ControllerMessage<'_>) {
    match *message {
        ControllerMessage::TradelaneRequest {
            ship,
            ring,
            end_ring,
            system,
        } => handle_request(cx, ship, ring, end_ring, system),
        ControllerMessage::TradelaneRelease { ship, ring, .. } => handle_release(cx, ship, ring),
        _ => {}
    }
}

/// The AI's trade-lane requests of the server, in the order the world
/// queued them ([`crate::dock::server_requests`]).
pub fn server_request(cx: &mut Cx<'_>, request: ServerRequest) {
    match request {
        ServerRequest::GoTradeLane {
            ship,
            entrance,
            exit,
            fast_exit,
            speed,
        } => ai_go_tradelane(cx, ship, entrance, exit, fast_exit, speed),
        ServerRequest::StopTradeLane {
            ship,
            entrance,
            exit,
        } => ai_stop_tradelane(cx, ship, entrance, exit),
        _ => {}
    }
}
