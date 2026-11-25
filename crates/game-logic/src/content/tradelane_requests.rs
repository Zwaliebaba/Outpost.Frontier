//! The trade-lane requests of TU55 (`Content/TradelaneRequests.cpp`), the
//! cases `TradelaneRequest` and `TradelaneRelease` of SpaceSupport's switch
//! (`Content/SpaceSupport.cpp:669-676`; work package P3.3, moved from
//! `crate::tradelane` into Content with P3.6a).
//!
//! An object that is not a lane ring (type bit 0x80) gets response 5; a
//! ring grants the lane with response 4 (`pub::SpaceObj::LaneResponse`,
//! which reaches the requesting behaviour through `Ship::request_returned`)
//! and is activated for the ship (`pub::SpaceObj::Activate(Ring, true,
//! Ship)`), unless the player's route check closes it (1). The cancel
//! deactivates the ring ([`handle_release`]).

use crate::server::Cx;

/// `DOCK_HOST_RESPONSE` 4: the lane is granted.
pub const LANE_GRANTED: i32 = 4;

/// `DOCK_HOST_RESPONSE` 5: refused (not a lane ring).
pub const LANE_REFUSED: i32 = 5;

/// `DOCK_HOST_RESPONSE` 1: the player's route is closed.
pub const LANE_ROUTE_CLOSED: i32 = 1;

/// The route check of the player whose ship object is `ship`
/// (`FindContentPlayerStateByShip`, then `CheckTradelaneRoute(ring,
/// end_ring)` over the rings of `ring`'s lane in the player's system
/// record; an unknown ring reads no rings); `None` for a ship that is no
/// player's.
fn route_check(cx: &Cx<'_>, ship: u32, ring: u32, end_ring: u32) -> Option<i32> {
    let content = &cx.logic.content.state;
    let player = super::players::find_by_ship(&content.players, ship)?;
    let state = content.players.get(&player)?;
    let lane: Vec<u32> = state
        .system
        .and_then(|s| content.systems.get(&s))
        .and_then(|r| r.lane_rings_of_ring(ring))
        .map(|rings| rings.iter().map(|r| r.object).collect())
        .unwrap_or_default();
    Some(state.check_tradelane_route(&lane, ring, end_ring))
}

/// `HandleTradelaneRequest` (SpaceSupport's message 0x14,
/// `Content/TradelaneRequests.cpp:24-66`): response 5 unless the object
/// asked for is a lane ring (`pub::SpaceObj::GetType`'s bit 0x80). For a
/// ring, a player's ship asks the player's route check
/// (`FContentPlayerState::CheckTradelaneRoute` over the rings of the ring's
/// lane in the player's system record): 1 and 2 answer 1; otherwise the
/// ring grants the lane (4). The answer goes to the ship
/// (`pub::SpaceObj::LaneResponse`), and a granted ring is activated for
/// it (`pub::SpaceObj::Activate(Ring, true, Ship)`). Before the answer a
/// grant queues the comm system's "granted" conversation (type 2,
/// [`super::space_support::gcs_lane_granted_event`]); an unknown ring, whose
/// type the C++ reads from a stale stack slot, reads 0 (D735).
pub fn handle_request(cx: &mut Cx<'_>, ship: u32, ring: u32, end_ring: u32, system: u32) {
    let ring_type = crate::api::space_obj::object_type(cx, ring).unwrap_or(0);
    let response = if ring_type & 0x80 != 0 {
        match route_check(cx, ship, ring, end_ring) {
            Some(1 | 2) => LANE_ROUTE_CLOSED,
            _ => LANE_GRANTED,
        }
    } else {
        LANE_REFUSED
    };
    if response == LANE_GRANTED {
        super::space_support::gcs_lane_granted_event(cx, system, ring, ship);
    }
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
