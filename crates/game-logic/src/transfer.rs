//! A ship's jump between systems (work package P3.3, decision record 0007
//! point 5): the end of a player's gate jump (`IServerImpl::
//! SystemSwitchOutComplete`, `Server/IServerImpl.cpp:1245`), the arrival
//! point a gate, a jump hole or an airlock gives (`Solar::
//! ChooseSystemArrivalLocation`, `Server/server_gameobject.cpp:6409`), and
//! the end of a jump-in (`IServerImpl::JumpInComplete`, `:1605`, and
//! `Ship::slot_74`, `server_gameobject.cpp:6170`).
//!
//! - **A player's gate jump.** Docking with a gate or a jump hole starts
//!   the ship's death sequence with the gate as its object and makes the
//!   ship invincible ([`crate::dock::complete_dock`]); the client plays the
//!   jump out and reports it. The server then finds the gate in the ship's
//!   system, its destination system (`goto`'s system) and arrival object
//!   (`goto`'s gate), tells the bus (`Msg0x25`), takes the ship out of the
//!   old system ([`crate::world::remove_ship`]), moves the player out of it
//!   (`player_switched_out`, which unloads it in single player) and into
//!   the destination (`add_player`, which loads it), puts the ship at the
//!   arrival object's exit point (`add_ship`, `arrive_ship`), sets the
//!   player's system and tells the bus (`SystemEnter`).
//! - **The jump-in's end.** The client's `JumpInComplete` for a player's
//!   ship, or the world's `JumpInAction` for an NPC's
//!   ([`game_sim::object::world::ServerHooks::jump_in_complete`]), tells the
//!   bus and ends the jump's invincibility.
//!
//! An NPC's jump goes through Content: `pub::SpaceObj::Relocate` to system
//! 0 and `JumpIn` at the destination gate ([`crate::api::space_obj`]). A
//! player's jump ends with the autosave of the player's slot
//! ([`crate::saves::auto_save`]).

use game_sim::object::{KindData, ObjHandle, ObjectKind};
use neuron_core::math::{Matrix, Transform, Vector, transform_xform};
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::objects::Invincibility;
use crate::server::Cx;

const SOLAR_CPP: &str = "Server\\Space\\Solar.cpp";
const ISERVER_S_CPP: &str = "Server\\space\\IServerS.cpp";

/// The type bits of a jump gate and a jump hole.
const GATE_BITS: u32 = 0x40 | 0x800;
/// The type bits of an airlock.
const AIRLOCK_BITS: u32 = 0x400;
/// `1 / 32768.0f`, which scales `rand()` to 0..1.
const RAND_UNIT: f32 = 1.0 / 32768.0;

/// `Server_FindLiveObject`: the object of an id that is not destroyed, and
/// its system (`None` while it is in transit).
#[must_use]
pub fn find_live(cx: &Cx<'_>, id: u32) -> Option<(ObjHandle, Option<u32>)> {
    let h = crate::dock::lookup_live(cx, id)?;
    let system = (!cx.world.in_transit(h)).then(|| cx.world.get(h).map(|o| o.physical.system));
    Some((h, system.flatten()))
}

/// `SysDB_LookupSystem`: a system id whose low 30 bits are not 0 and that
/// has an entry.
#[must_use]
pub fn lookup_system(cx: &mut Cx<'_>, id: u32) -> Option<u32> {
    crate::world::initialize(cx);
    (id & 0x3fff_ffff != 0 && cx.logic.server_world.systems.contains_key(&id)).then_some(id)
}

/// `Solar::ChooseSystemArrivalLocation`: a gate or a jump hole (type bits
/// 0x40 or 0x800) puts the arriving ship on a 500 m ring 350 m behind it
/// (`rand()`'s angle, the local point `(cos·500, sin·500, -350)` through the
/// solar's transform) with the solar's orientation; an airlock (0x400) at
/// the solar's position turned half round its local Y axis; anything else
/// is reported and gives `(35000, 0, 35000)` with the identity. An object
/// the world does not have gives the latter without the report.
pub fn choose_system_arrival_location(cx: &mut Cx<'_>, solar: ObjHandle) -> (Vector, Matrix) {
    let fallback = (Vector::new(35000.0, 0.0, 35000.0), Matrix::IDENTITY);
    let Some((bits, position, rotation)) = cx
        .world
        .get(solar)
        .map(|o| (o.simple.type_bits, o.engine.position, o.engine.orientation))
    else {
        return fallback;
    };
    if bits & GATE_BITS != 0 {
        let random = cx.world.rng().rand();
        // `float(rand()) * (1 / 32768.0f)` widened, times the double of the
        // float 2π (6.2831854820251465).
        let angle = f64::from(random as f32 * RAND_UNIT) * f64::from(std::f32::consts::TAU);
        let local = Vector::new(
            (angle.cos() * 500.0) as f32,
            (angle.sin() * 500.0) as f32,
            -350.0,
        );
        let out = transform_xform(&Transform::new(rotation, position), local);
        return (out, rotation);
    }
    if bits & AIRLOCK_BITS != 0 {
        let mut orientation = rotation;
        // The double of the float π (3.1415927410125732).
        let pi = f64::from(std::f32::consts::PI);
        let (c, s) = (pi.cos(), pi.sin());
        for column in 0..3 {
            let row0 = f64::from(orientation.m[0][column]);
            let row2 = f64::from(orientation.m[2][column]);
            orientation.m[0][column] = (row0 * c + s * row2) as f32;
            orientation.m[2][column] = (c * row2 - row0 * s) as f32;
        }
        return (position, orientation);
    }
    cx.logic.reports.push(
        SOLAR_CPP,
        0xeb,
        "*** ERROR: Invalid type in ChooseSystemArrivalLocation()".to_owned(),
    );
    fallback
}

/// `StarSystem::find_object(Id, 0xc40)`: the mask selects the solar list,
/// so the solar of the system with the id.
fn find_jump_object(cx: &Cx<'_>, system: u32, id: u32) -> Option<ObjHandle> {
    if id & 0x3fff_ffff == 0 {
        return None;
    }
    let h = cx.world.find(id)?;
    let o = cx.world.get(h)?;
    (o.kind == ObjectKind::Solar && !cx.world.in_transit(h) && o.physical.system == system)
        .then_some(h)
}

/// `IServerImpl::SystemSwitchOutComplete(Ship, Player)`: a player's ship
/// has jumped out through the gate or jump hole of its death sequence.
///
/// For a valid session in space with a ship: the ship (destroyed or not)
/// in a system, owned by the session; its gate (the death sequence's
/// object, a solar of that system); the gate's destination system, which
/// must exist (else reported). Then `Msg0x25` (the ship, the gate, the
/// system left) goes to the bus, the ship leaves the system
/// ([`crate::world::remove_ship`]), the player leaves it
/// ([`crate::world::player_switched_out`], unloading it in single player)
/// and joins the destination ([`crate::world::add_player`], loading it);
/// the destination's solar named by the gate's `goto` gives the arrival
/// pose ([`choose_system_arrival_location`]; without one it is reported
/// and the origin with the identity is used); the ship enters there
/// ([`crate::world::add_ship`], [`crate::world::arrive_ship`]), the
/// player's system becomes the destination, and `SystemEnter` (the
/// destination, the old system, the player, the arrival object and the
/// position) goes to the bus. Without an arrival object the C++ faults
/// reading its id; the message carries 0 (D683). The player's slot is then
/// autosaved ([`crate::saves::auto_save`]), in single player as on a
/// multiplayer server.
pub fn system_switch_out_complete(cx: &mut Cx<'_>, ship_id: u32, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    if slot.current_base & 0x3fff_ffff != 0 || slot.ship_object_id & 0x3fff_ffff == 0 {
        return;
    }
    // `Server_FindObject`: a destroyed ship is found too; one in transit
    // has no system.
    let Some(ship) = cx.world.find(ship_id) else {
        return;
    };
    if cx.world.in_transit(ship) {
        return;
    }
    let Some((system, owner)) = cx
        .world
        .get(ship)
        .map(|o| (o.physical.system, o.simple.owner_player))
    else {
        return;
    };
    if owner != session {
        return;
    }
    let gate_id = crate::ship::ship_state(cx, ship).map_or(0, |s| s.death_sequence_obj_id);
    let Some(gate) = find_jump_object(cx, system, gate_id) else {
        return;
    };
    let Some((dest_system, dest_gate)) = cx.world.get(gate).and_then(|o| match &o.data {
        KindData::Solar(s) => Some((s.dest_system, s.dest_gate)),
        _ => None,
    }) else {
        return;
    };
    if lookup_system(cx, dest_system).is_none() {
        cx.logic.reports.push(
            ISERVER_S_CPP,
            0x6b,
            "*** ERROR: FATAL: Couldn't find gate destination system!".to_owned(),
        );
        return;
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::Msg0x25 {
            ship: ship_id,
            death_sequence: gate_id,
            system,
        },
    );
    let _ = crate::world::remove_ship(cx, ship);
    crate::world::player_switched_out(cx, system, session, dest_system);
    crate::world::add_player(cx, dest_system, session);
    let arrival = crate::dock::find_solar(cx, dest_system, dest_gate);
    let (position, orientation) = match arrival {
        Some(h) => choose_system_arrival_location(cx, h),
        None => {
            cx.logic.reports.push(
                ISERVER_S_CPP,
                0x8d,
                format!("*** ERROR: Couldn't find arrival gate {dest_gate:08X}."),
            );
            (Vector::ZERO, Matrix::IDENTITY)
        }
    };
    crate::world::add_ship(cx, dest_system, ship, position, orientation);
    crate::world::arrive_ship(cx, dest_system, ship, arrival, position, orientation);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.system_id = dest_system;
    }
    let arrival_id = arrival
        .and_then(|h| cx.world.get(h))
        .map_or(0, |o| o.simple.id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SystemEnter {
            system: dest_system,
            from: system,
            player: session,
            arrival: arrival_id,
            position,
        },
    );
    let _ = crate::saves::auto_save(cx, session);
}

/// `IServerImpl::JumpInComplete(Arg, Object)`: the client's report that
/// its ship's jump-in ended; the object, destroyed or not, gets
/// `IObjAffect::slot_74(Arg)` ([`slot_74`]).
pub fn jump_in_complete(cx: &mut Cx<'_>, arg: u32, object_id: u32) {
    if let Some(h) = cx.world.find(object_id) {
        let _ = slot_74(cx, h, arg);
    }
}

/// `IObjAffect::slot_74(Arg)`: on a ship (`Ship::slot_74`) `JumpInComplete`
/// (the argument, the ship) goes to the bus and the jump's invincibility
/// (type 2) ends: no protection, from no one, the whole of the hit points
/// may go. 0; any other object answers -3 (`GameObject::slot_74`).
pub fn slot_74(cx: &mut Cx<'_>, h: ObjHandle, arg: u32) -> i32 {
    let Some(ship) = cx
        .world
        .get(h)
        .filter(|o| o.kind == ObjectKind::Ship && o.equipped.is_some())
        .map(|o| o.simple.id)
    else {
        return -3;
    };
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::JumpInComplete { arg, ship },
    );
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.set_invincible(Invincibility {
            kind: 2,
            protect: false,
            from_players: false,
            max_hp_loss: 1.0,
        });
    }
    0
}
