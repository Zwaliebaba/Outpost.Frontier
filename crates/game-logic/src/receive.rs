//! The client's packets into the server (`IServer`): a message a client
//! sent, decoded from channel 1 or 3 ([`ToServer`]), becomes the
//! `IServerImpl` call the C++'s receivers make of it, with the session it
//! came from. In multiplayer they are `RemoteClient`'s `DispatchClientPacket`
//! (`RemoteClient/RemoteClient_main.cpp:18`), in single player rpclocal's
//! channel-1 and channel-3 delivery (`rpclocal/rpc_payload_ch3.cpp`); both
//! read the same fields for the packets routed here.
//!
//! | Packet | `IServer` slot | Port |
//! | --- | --- | --- |
//! | `FLPACKET_COMMON_UPDATEOBJECT` (ch1 0x01) | 52 `SPObjUpdate(Info, Session)` | [`crate::replication::client_object_update`] |
//! | `FLPACKET_COMMON_FIREWEAPON` (ch1 0x02) | 0 `FireWeapon(Session, Info)` | [`fire_weapon`] |
//! | `FLPACKET_COMMON_SETTARGET` (ch1 0x04) | 4 `SetTarget(Session, Info)` | [`set_target`] |
//! | `FLPACKET_COMMON_ACTIVATEEQUIP` (ch1 0x08) | 1 `ActivateEquip(Session, Info)` | [`activate_equip`] |
//! | `FLPACKET_COMMON_ACTIVATECRUISE` (ch1 0x0e) | 2 `ActivateCruise(Session, Info)` | [`activate_cruise`] |
//! | `FLPACKET_COMMON_GOTRADELANE` (ch1 0x0f) | 6 `GoTradelane(Session, Info)` | [`go_tradelane`] |
//! | `FLPACKET_COMMON_STOPTRADELANE` (ch1 0x10) | 7 `StopTradelane(Session, Ship, Ring1, Ring2)` | [`stop_tradelane`] |
//! | `FLPACKET_COMMON_JETTISONCARGO` (ch1 0x14) | 8 `JettisonCargo(Session, Info)` | [`jettison_cargo`] |
//! | `FLPACKET_COMMON_ACTIVATETHRUSTERS` (ch1 0x15) | 3 `ActivateThrusters(Session, Info)` | [`activate_thrusters`] |
//! | `FLPACKET_COMMON_TRACTOROBJECTS` (ch1 0x1d) | 5 `TractorObjects(Session, Info)` | [`tractor_objects`] |
//! | `FLPACKET_CLIENT_MUNCOLLISION` (ch3 0x03) | 53 `SPMunitionCollision(Info, Session)` | [`munition_collision`] |
//! | `FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE` (ch3 0x0b) | 47 `SystemSwitchOutComplete(Ship, Session)` | [`crate::transfer::system_switch_out_complete`] |
//! | `FLPACKET_CLIENT_OBJCOLLISION` (ch3 0x0c) | 55 `SPObjCollision(Info, Session)` | [`object_collision`] |
//! | `FLPACKET_CLIENT_MINEASTEROID` (ch3 0x20) | `MineAsteroid(System, Pos, Loot, Secondary, Count, Session)` | [`mine_asteroid`] |
//! | `FLPACKET_CLIENT_DOCK` (ch3 0x24) | 91 `Dock(Base, Session)` | [`crate::dock::force_land`] |
//! | `FLPACKET_CLIENT_SETMANEUVER` (ch3 0x2f) | `SetManeuver(Session, Info)` | [`set_maneuver`] |
//! | `FLPACKET_CLIENT_BADLANDSOBJCOLLISION` (ch3 0x3d) | 54 `SPBadLandsObjCollision(Info, Session)` | [`badlands_collision`] |
//! | `FLPACKET_CLIENT_REQUEST_USE_ITEM` (ch3 0x40) | 56 `SPRequestUseItem(Info, Session)` | [`request_use_item`] |
//! | `FLPACKET_CLIENT_SKIP_AUTOSAVE` (ch3 0x42) | `CharacterSkipAutosave(Session)` | [`crate::saves::character_skip_autosave`] |
//! | `FLPACKET_CLIENT_JUMPINCOMPLETE` (ch3 0x43) | `JumpInComplete(Field_0x00, Field_0x04)`, no session | [`crate::transfer::jump_in_complete`] |
//! | `FLPACKET_CLIENT_REQINVINCIBILITY` (ch3 0x44) | `SPRequestInvincibility(Ship, On, Reason, Session)` | [`request_invincibility`] |
//! | `FLPACKET_CLIENT_MISSIONSAVEB` (ch3 0x45) | `MissionSaveB(Session, Value)` | [`crate::saves::mission_save_b`] |
//!
//! The flight and combat handlers (work package P3.10a,
//! `Server/IServerImpl.cpp:412-575`, `1694-2027`, `2756`) look the object
//! up by id (`Server_LookupGameObject`, destroyed or not; or
//! `Server_FindObject` with its system, none in transit), refuse a
//! destroyed one where the C++ does, and call the server wrapper's
//! `IObjAffect` slot with the session as the source client: the slot
//! applies the request (on a multiplayer server; in single player the
//! client already did) and tells every other player that can see the
//! object. An object of a class without the slot answers -3 and does
//! nothing (`GameObject`'s defaults). `FireWeapon` and `JettisonCargo`
//! end the player's trade first, `SPRequestUseItem` after a use that
//! succeeded (`PlayerData_TerminateTrade`,
//! [`crate::trade::player_terminate_trade`]).
//!
//! The player-state requests and the trades of channel 1 and the item
//! requests of channel 3 (work package P3.10b) are [`crate::session`]'s
//! and [`crate::trade`]'s.
//!
//! [`deliver`] answers false for the packets the session's entry points,
//! the chat and the player-state requests take ([`crate::session::route`],
//! [`crate::session::route_common`]). `FLPACKET_CLIENT_SAVEGAME` is the
//! session's: its single-player form carries the character id that
//! rpclocal's payload holds and the wire leaves out, and the session hands
//! it to [`crate::saves::server_save_game`].

use game_sim::equip::SUBOBJ_ID_NONE;
use game_sim::object::{ObjHandle, ObjectKind, class};
use game_sim::protocol::common::{
    ActivateCruise, ActivateEquip, ActivateThrusters, FireWeapon, GoTradeLane, JettisonCargo,
    SetTarget, StopTradeLane, TractorObjects, UpdateObject,
};
use game_sim::protocol::to_server::{
    BadlandsObjCollision, MineAsteroid, MunCollision, ObjCollision, RequestUseItem, SetManeuver,
};
use game_sim::protocol::{ClientMessage, CommonMessage, ToServer, expand_orientation};
use neuron_core::math::{Quaternion, Vector};
use neuron_core::net::wire::quantise::expand_unit;
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::objects::Invincibility;
use crate::replication::ObjectUpdate;
use crate::server::Cx;

/// `IServerImpl`'s handling of one message from the client of `session`:
/// true when the message was routed to its server function, false for a
/// message this module does not route yet.
pub fn deliver(cx: &mut Cx<'_>, session: u32, message: &ToServer) -> bool {
    match message {
        ToServer::Common(message) => deliver_common(cx, session, message),
        ToServer::Client(message) => deliver_client(cx, session, message),
    }
}

/// `DispatchClientPacket`'s channel 1.
fn deliver_common(cx: &mut Cx<'_>, session: u32, message: &CommonMessage) -> bool {
    match message {
        CommonMessage::UpdateObject(packet) => {
            crate::replication::client_object_update(cx, &object_update(packet), session);
        }
        CommonMessage::FireWeapon(packet) => fire_weapon(cx, session, packet),
        CommonMessage::SetTarget(packet) => set_target(cx, session, packet),
        CommonMessage::ActivateEquip(packet) => activate_equip(cx, session, packet),
        CommonMessage::ActivateCruise(packet) => activate_cruise(cx, session, packet),
        CommonMessage::GoTradeLane(packet) => go_tradelane(cx, session, packet),
        CommonMessage::StopTradeLane(packet) => stop_tradelane(cx, session, packet),
        CommonMessage::JettisonCargo(packet) => jettison_cargo(cx, session, packet),
        CommonMessage::ActivateThrusters(packet) => activate_thrusters(cx, session, packet),
        CommonMessage::TractorObjects(packet) => tractor_objects(cx, session, packet),
        _ => return false,
    }
    true
}

/// `DispatchClientPacket`'s channel 3.
fn deliver_client(cx: &mut Cx<'_>, session: u32, message: &ClientMessage) -> bool {
    match message {
        ClientMessage::SystemSwitchOutComplete(packet) => {
            crate::transfer::system_switch_out_complete(cx, packet.value, session);
        }
        ClientMessage::JumpInComplete(packet) => {
            crate::transfer::jump_in_complete(cx, packet.field_00, packet.field_04);
        }
        ClientMessage::SkipAutosave(_) => crate::saves::character_skip_autosave(cx, session),
        ClientMessage::MissionSaveB(packet) => {
            crate::saves::mission_save_b(cx, session, packet.value);
        }
        ClientMessage::MunCollision(packet) => munition_collision(cx, session, packet),
        ClientMessage::ObjCollision(packet) => object_collision(cx, session, packet),
        ClientMessage::BadlandsObjCollision(packet) => badlands_collision(cx, session, packet),
        ClientMessage::MineAsteroid(packet) => mine_asteroid(cx, session, packet),
        ClientMessage::Dock(packet) => crate::dock::force_land(cx, packet.value, session),
        ClientMessage::SetManeuver(packet) => set_maneuver(cx, session, packet),
        ClientMessage::RequestUseItem(packet) => request_use_item(cx, session, packet),
        ClientMessage::ReqInvincibility(packet) => request_invincibility(
            cx,
            packet.ship_id,
            packet.invincible,
            u32::from(packet.reason),
            session,
        ),
        _ => return false,
    }
    true
}

/// `SSPObjUpdateInfo` from a decoded `UPDATEOBJECT`, as
/// `DispatchClientPacket` fills it: the id, the position, the
/// orientation's four bytes (`x`, `y`, `z`, `w`) and the fifth each as a
/// signed byte × 1/127 stored to a float (`FRpcUpdateObjectPacket::Decode`,
/// `RpcUpdateExpand`), the update time widened to the timestamp's double,
/// the fifth value's float bits in `Field_0x28` and the first byte in
/// `Field_0x2c`.
#[must_use]
pub fn object_update(packet: &UpdateObject) -> ObjectUpdate {
    let [w, x, y, z] = expand_orientation(packet.orientation);
    let [px, py, pz] = packet.position;
    ObjectUpdate {
        object_id: packet.object_id,
        orientation: Quaternion { w, x, y, z },
        position: Vector::new(px, py, pz),
        timestamp: f64::from(packet.update_time),
        field_28: expand_unit(packet.extra).to_bits(),
        field_2c: packet.flags,
    }
}

// ---------------------------------------------------------------------------
// The lookups and narrowings the handlers share.

/// The session as the `int` source client the slots take.
fn source(session: u32) -> i32 {
    i32::from_ne_bytes(session.to_ne_bytes())
}

/// `Server_LookupGameObject` with the handlers' `is_destroyed` test: the
/// id's object unless it is destroyed (the server's latch or dead in the
/// world).
fn live_object(cx: &Cx<'_>, id: u32) -> Option<ObjHandle> {
    let h = cx.world.find(id)?;
    let destroyed = cx.logic.objects.get(h).is_some_and(|o| o.destroyed)
        || cx.world.get(h).is_none_or(|o| o.dead);
    (!destroyed).then_some(h)
}

/// `Server_FindObject` whose system out is set: the id's object, destroyed
/// or not, and its system; `None` for an unknown id or an object in
/// transit (its system out is 0).
fn object_in_system(cx: &Cx<'_>, id: u32) -> Option<(ObjHandle, u32)> {
    let h = cx.world.find(id)?;
    if cx.world.in_transit(h) {
        return None;
    }
    let system = cx.world.get(h)?.physical.system;
    Some((h, system))
}

/// The `EqObj` wrapper (`CEqObj`, class `& 0x103`): a ship or a solar with
/// equipment.
fn is_eq_obj(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.is(class::EQ_OBJ) && o.equipped.is_some())
}

/// The `Ship` wrapper.
fn is_ship(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.kind == ObjectKind::Ship && o.equipped.is_some())
}

/// The slot's ship id (`Players[Session].ship_object_id`, the raw slot);
/// none for a session without a slot, which the C++ reads past the table
/// (D933).
fn slot_ship(cx: &Cx<'_>, session: u32) -> Option<u32> {
    cx.logic.players.get(session).map(|p| p.ship_object_id)
}

// ---------------------------------------------------------------------------
// The handlers.

/// `IServerImpl::FireWeapon(Session, Info)` (`IServerImpl.cpp:412`): for
/// a live object, the session's trade ends, then the object's
/// `EqObj::slot_34` with the hardpoints, the aim point and
/// the session ([`crate::equip_fire::fire_weapons`]; a client's shot pays
/// its ammunition and launches only its seeking munitions on a
/// multiplayer server, and the other players hear of the unguided ones).
/// The aim point is the multiplayer encoder's components as the receiver
/// expands them.
pub fn fire_weapon(cx: &mut Cx<'_>, session: u32, packet: &FireWeapon) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    crate::trade::player_terminate_trade(cx, session);
    let [x, y, z] = packet
        .target
        .map(game_sim::protocol::common::FireComponent::value);
    if is_eq_obj(cx, h) {
        crate::equip_fire::fire_weapons(
            cx,
            h,
            &packet.hardpoints,
            Some(Vector::new(x, y, z)),
            source(session),
        );
    }
}

/// `IServerImpl::ActivateEquip(Session, Info)` (`IServerImpl.cpp:429`): a
/// live object's `EqObj::slot_4c` ([`crate::equip_fire::activate_equip`]),
/// any non-zero byte switching the item on.
pub fn activate_equip(cx: &mut Cx<'_>, session: u32, packet: &ActivateEquip) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    if is_eq_obj(cx, h) {
        crate::equip_fire::activate_equip(
            cx,
            h,
            packet.equip_id,
            packet.active != 0,
            source(session),
        );
    }
}

/// `IServerImpl::ActivateCruise(Session, Info)` (`IServerImpl.cpp:442`): a
/// live object's `Ship::slot_50` with both bytes as flags
/// ([`crate::equip_fire::engage_cruise`], which refuses anything but a
/// ship).
pub fn activate_cruise(cx: &mut Cx<'_>, session: u32, packet: &ActivateCruise) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    crate::equip_fire::engage_cruise(
        cx,
        h,
        packet.active != 0,
        packet.field_05 != 0,
        source(session),
    );
}

/// `IServerImpl::ActivateThrusters(Session, Info)` (`IServerImpl.cpp:455`):
/// a live ship's `Ship::slot_54`
/// ([`crate::equip_fire::set_thrusters_active`]).
pub fn activate_thrusters(cx: &mut Cx<'_>, session: u32, packet: &ActivateThrusters) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    if is_ship(cx, h) {
        crate::equip_fire::set_thrusters_active(cx, h, packet.active != 0, source(session));
    }
}

/// `IServerImpl::SetTarget(Session, Info)` (`IServerImpl.cpp:474`): for a
/// live object, the target looked up by id (none when unknown, destroyed
/// or not). Without a sub-object (`SUBOBJ_ID_NONE`) it is the object's own
/// target: `Ship::set_target` ([`crate::equip_fire::set_target`]) or
/// `Guided::set_target` ([`crate::munitions::set_target`]); with one it is
/// that gun's (`EqObj::slot_44`, [`crate::equip_fire::set_gun_target`]).
pub fn set_target(cx: &mut Cx<'_>, session: u32, packet: &SetTarget) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    let target = cx.world.find(packet.target_id);
    let sub_target = packet.target_sub_obj_id;
    if packet.sub_obj_id == SUBOBJ_ID_NONE {
        if is_ship(cx, h) {
            crate::equip_fire::set_target(cx, h, target, sub_target, source(session));
        } else if cx
            .world
            .get(h)
            .is_some_and(|o| o.kind == ObjectKind::Guided)
        {
            crate::munitions::set_target(cx, h, target, sub_target, source(session));
        }
        return;
    }
    if is_eq_obj(cx, h) {
        crate::equip_fire::set_gun_target(
            cx,
            h,
            packet.sub_obj_id,
            target,
            sub_target,
            source(session),
        );
    }
}

/// `IServerImpl::TractorObjects(Session, Info)` (`IServerImpl.cpp:503`):
/// for a live object, the targets the server knows (in order, unknown ids
/// left out) through `EqObj::slot_3c`
/// ([`crate::equip_fire::tractor_objects`]).
pub fn tractor_objects(cx: &mut Cx<'_>, session: u32, packet: &TractorObjects) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    let targets: Vec<ObjHandle> = packet
        .target_ids
        .iter()
        .filter_map(|&id| cx.world.find(id))
        .collect();
    if is_eq_obj(cx, h) {
        crate::equip_fire::tractor_objects(
            cx,
            h,
            packet.tractor_equip_id,
            &targets,
            source(session),
        );
    }
}

/// `IServerImpl::GoTradelane(Session, Info)` (`IServerImpl.cpp:530`): only
/// the session's own ship, found in a system (not in transit), enters the
/// lane: `StarSystem::notify_go_tradelane` with the rings, the fast-exit
/// byte and the speed 0 ([`crate::replication::go_tradelane`]: the bus,
/// the lane flag, the NPC ships' behaviours and the other players).
pub fn go_tradelane(cx: &mut Cx<'_>, session: u32, packet: &GoTradeLane) {
    if slot_ship(cx, session) != Some(packet.object_id) {
        return;
    }
    let Some((_, system)) = object_in_system(cx, packet.object_id) else {
        return;
    };
    crate::replication::go_tradelane(
        cx,
        system,
        packet.object_id,
        packet.tradelane_ring1,
        packet.tradelane_ring2,
        packet.field_0c != 0,
    );
}

/// `IServerImpl::StopTradelane(Session, Ship, Ring1, Ring2)`
/// (`IServerImpl.cpp:547`): as [`go_tradelane`], with
/// `StarSystem::notify_stop_tradelane`
/// ([`crate::replication::stop_tradelane`]).
pub fn stop_tradelane(cx: &mut Cx<'_>, session: u32, packet: &StopTradeLane) {
    if slot_ship(cx, session) != Some(packet.ship_id) {
        return;
    }
    let Some((_, system)) = object_in_system(cx, packet.ship_id) else {
        return;
    };
    crate::replication::stop_tradelane(
        cx,
        system,
        packet.ship_id,
        packet.tradelane_ring1,
        packet.tradelane_ring2,
    );
}

/// `IServerImpl::JettisonCargo(Session, Info)` (`IServerImpl.cpp:562`):
/// for a live object, the session's trade ends, then a ship's
/// `Ship::slot_38` with the item and the count as an `int`
/// ([`crate::cargo::jettison`]).
pub fn jettison_cargo(cx: &mut Cx<'_>, session: u32, packet: &JettisonCargo) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    crate::trade::player_terminate_trade(cx, session);
    if is_ship(cx, h) {
        let count = i32::from_ne_bytes(packet.count.to_ne_bytes());
        crate::cargo::jettison(cx, h, packet.item_id, count, source(session));
    }
}

/// `IServerImpl::SPMunitionCollision(Info, Session)` (`IServerImpl.cpp:
/// 1694`): for a valid session in a ship whose relative health is above
/// 0.0001 (a NaN passes), the object hit, found in a system, takes the
/// munition (`StarSystem::munition_collision`,
/// [`crate::world::munition_collision`]).
pub fn munition_collision(cx: &mut Cx<'_>, session: u32, packet: &MunCollision) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    if slot.ship_object_id & 0x3fff_ffff == 0 {
        return;
    }
    let health = f64::from(slot.relative_health);
    if health <= 0.0001 {
        return;
    }
    let Some((_, system)) = object_in_system(cx, packet.object_id) else {
        return;
    };
    let [x, y, z] = packet.hit_position;
    let hit = crate::world::MunitionHit {
        munition: packet.field_00,
        shooter: packet.field_04,
        object: packet.object_id,
        sub_obj: packet.field_0c,
        position: Vector::new(x, y, z),
    };
    crate::world::munition_collision(cx, system, &hit);
}

/// `IServerImpl::SPObjCollision(Info, Session)` (`IServerImpl.cpp:1739`):
/// the record is `FCollisionDamage` as it stands: the other object
/// (`TargetObjectId`) is the source, the reported object (`ObjectId`),
/// found in a system, the target, its sub-object the one hit and
/// `Field_0x10` the damage (`StarSystem::collision_damage`,
/// [`crate::world::collision_damage`]). The session is not read.
pub fn object_collision(cx: &mut Cx<'_>, session: u32, packet: &ObjCollision) {
    let _ = session;
    let Some((h, system)) = object_in_system(cx, packet.object_id) else {
        return;
    };
    crate::world::collision_damage(
        cx,
        system,
        packet.target_object_id,
        h,
        packet.sub_obj_id,
        packet.field_10,
    );
}

/// `IServerImpl::SPBadLandsObjCollision(Info, Session)`
/// (`IServerImpl.cpp:1717`): the object, found in a system, meets the
/// badlands (`StarSystem::badlands_collision`,
/// [`crate::world::badlands_collision`]: destroyed by `Field_0x00` unless
/// invincible). The session is not read.
pub fn badlands_collision(cx: &mut Cx<'_>, session: u32, packet: &BadlandsObjCollision) {
    let _ = session;
    let Some((_, system)) = object_in_system(cx, packet.object_id) else {
        return;
    };
    crate::world::badlands_collision(cx, system, packet.object_id, packet.field_00);
}

/// `IServerImpl::MineAsteroid(System, Pos, Loot, Secondary, Count,
/// Session)` (`IServerImpl.cpp:1914`): the system's mined loot
/// ([`crate::loot::spawn_mined_loot`], the loot as the container and the
/// secondary loot as the contents). The session is not read; a system the
/// server does not have, which the C++ reads through null, spawns nothing
/// (D931).
pub fn mine_asteroid(cx: &mut Cx<'_>, session: u32, packet: &MineAsteroid) {
    let _ = session;
    let Some(system) = crate::transfer::lookup_system(cx, packet.system_id) else {
        return;
    };
    let [x, y, z] = packet.position;
    crate::loot::spawn_mined_loot(
        cx,
        system,
        Vector::new(x, y, z),
        packet.loot_id,
        packet.secondary_loot,
        i32::from_ne_bytes(packet.quantity.to_ne_bytes()),
    );
}

/// `IServerImpl::SetManeuver(Session, Info)` (`IServerImpl.cpp:2002`): an
/// object with equipment the server knows (destroyed or not, in transit
/// too) gives its behaviour manager the type-2 event of the manoeuvre
/// (`IBehaviorManager::Slot00`, [`game_sim::object::World::maneuver_event`]:
/// a directive waiting for that behaviour ends); in single player
/// `PlayerManeuver` (the session, the object, the behaviour) goes to the
/// bus.
pub fn set_maneuver(cx: &mut Cx<'_>, session: u32, packet: &SetManeuver) {
    let Some(h) = cx.world.find(packet.object_id) else {
        return;
    };
    if !is_eq_obj(cx, h) {
        return;
    }
    cx.world
        .maneuver_event(h, i32::from_ne_bytes(packet.field_08.to_ne_bytes()));
    if cx.world.config().single_player {
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::PlayerManeuver {
                player: session,
                object: packet.object_id,
                value: packet.field_08,
            },
        );
    }
}

/// `IServerImpl::SPRequestUseItem(Info, Session)` (`IServerImpl.cpp:1766`):
/// a live object's `EqObj::slot_58` with the item and the count
/// ([`crate::cargo::use_item`]); the session's trade ends only when the
/// use succeeded (0).
pub fn request_use_item(cx: &mut Cx<'_>, session: u32, packet: &RequestUseItem) {
    let Some(h) = live_object(cx, packet.object_id) else {
        return;
    };
    if !is_eq_obj(cx, h) {
        return;
    }
    if crate::cargo::use_item(cx, h, packet.item_id, u32::from(packet.count)) == 0 {
        crate::trade::player_terminate_trade(cx, session);
    }
}

/// `IServerImpl::SPRequestInvincibility(Ship, On, Reason, Session)`
/// (`IServerImpl.cpp:1798`): for a valid session whose ship is `ship`, the
/// command `{Reason, On, On, loss}`: switching it off loses up to 1;
/// switching it on loses nothing and needs reason 5, or reason 2 with the
/// ship in the dock queue of one of the dockable solars of the slot's
/// system (`collect_dockable_solars(_, 0)`, `is_ship_queued`); any other
/// reason is refused. A live ship then takes the command
/// (`GameObject::set_invincible`).
pub fn request_invincibility(cx: &mut Cx<'_>, ship: u32, on: u8, reason: u32, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    if slot.ship_object_id != ship {
        return;
    }
    let slot_system = slot.system_id;
    let mut command = Invincibility {
        kind: reason,
        protect: on != 0,
        from_players: on != 0,
        max_hp_loss: 1.0,
    };
    if on != 0 {
        command.max_hp_loss = 0.0;
        match reason {
            2 => {
                let Some(system) = crate::transfer::lookup_system(cx, slot_system) else {
                    return;
                };
                let queued = crate::dock::collect_dockable_solars(cx, system, 0)
                    .into_iter()
                    .any(|solar| {
                        cx.logic
                            .objects
                            .get(solar)
                            .is_some_and(|o| o.dock_queue.iter().any(|r| r.ship == ship))
                    });
                if !queued {
                    return;
                }
            }
            5 => {}
            _ => return,
        }
    }
    let Some(h) = live_object(cx, ship) else {
        return;
    };
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.set_invincible(command);
    }
}
