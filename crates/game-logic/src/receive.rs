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
//! | `FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE` (ch3 0x0b) | 47 `SystemSwitchOutComplete(Ship, Session)` | [`crate::transfer::system_switch_out_complete`] |
//! | `FLPACKET_CLIENT_JUMPINCOMPLETE` (ch3 0x43) | `JumpInComplete(Field_0x00, Field_0x04)`, no session | [`crate::transfer::jump_in_complete`] |
//! | `FLPACKET_CLIENT_SKIP_AUTOSAVE` (ch3 0x42) | `CharacterSkipAutosave(Session)` | [`crate::saves::character_skip_autosave`] |
//! | `FLPACKET_CLIENT_MISSIONSAVEB` (ch3 0x45) | `MissionSaveB(Session, Value)` | [`crate::saves::mission_save_b`] |
//!
//! The other packets reach the server functions that exist as those
//! functions' callers arrive; until then [`deliver`] answers false for them.
//! `FLPACKET_CLIENT_SAVEGAME` is one of them: its decoded form carries only
//! the description, while `IServerImpl::SaveGame` names the save by the
//! character id that rpclocal's single-player payload holds and the wire
//! leaves out; [`crate::saves::server_save_game`] takes it from the caller.

use game_sim::protocol::common::UpdateObject;
use game_sim::protocol::{ClientMessage, CommonMessage, ToServer, expand_orientation};
use neuron_core::math::{Quaternion, Vector};
use neuron_core::net::wire::quantise::expand_unit;

use crate::replication::ObjectUpdate;
use crate::server::Cx;

/// `IServerImpl`'s handling of one message from the client of `session`:
/// true when the message was routed to its server function, false for a
/// message this module does not route yet.
pub fn deliver(cx: &mut Cx<'_>, session: u32, message: &ToServer) -> bool {
    match message {
        ToServer::Common(CommonMessage::UpdateObject(packet)) => {
            crate::replication::client_object_update(cx, &object_update(packet), session);
            true
        }
        ToServer::Client(ClientMessage::SystemSwitchOutComplete(packet)) => {
            crate::transfer::system_switch_out_complete(cx, packet.value, session);
            true
        }
        ToServer::Client(ClientMessage::JumpInComplete(packet)) => {
            crate::transfer::jump_in_complete(cx, packet.field_00, packet.field_04);
            true
        }
        ToServer::Client(ClientMessage::SkipAutosave(_)) => {
            crate::saves::character_skip_autosave(cx, session);
            true
        }
        ToServer::Client(ClientMessage::MissionSaveB(packet)) => {
            crate::saves::mission_save_b(cx, session, packet.value);
            true
        }
        _ => false,
    }
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
