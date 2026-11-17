//! The decode direction of the protocol oracle (work package P2.2): what a
//! packet the C++ decoder has read encodes to again.
//!
//! `CppSource/Oracle/oracle_protocol_packets.cpp` decodes every encoding
//! with the real decoder (in the receiver's mode) into an empty packet the
//! factory registry makes, and encodes that packet once more. The Rust
//! messages hold wire values, so the Rust decoder's message re-encodes to
//! the bytes it came from; the C++ packets hold engine values, and its
//! decoders convert where the wire quantises or looks an archetype up. The
//! functions here apply those conversions to a message the Rust decoder
//! read, so its encoding is what the C++ round trip writes when both
//! decoders read the same fields:
//!
//! - a × 127 byte becomes `quantise_unit(expand_unit(byte))` (quaternions,
//!   throttle, attitude, `UPDATEOBJECT`'s fifth value), a hull byte
//!   `quantise_hull(expand_hull(byte))` with the maximum of an unknown
//!   archetype, a × 255 byte `quantise_fraction(expand_fraction(byte))`, the
//!   `SENDCOMM` duration `quantise_tenths(expand_tenths(byte))`, and a
//!   `FIREWEAPON` component the tier and value the encoder picks for its
//!   expanded value;
//! - a small archetype ID becomes 0: no archetype is loaded in the harness,
//!   so `SmallIDToLargeID` gives 0 and `LargeIDToSmallID(0)` 0;
//! - a create-object equipment record whose health byte expands to 1.0 is
//!   sent as full health, a hardpoint text in the dictionary as its code,
//!   an unknown code or an empty text as no hardpoint;
//! - `CREATESHIP`'s owner flag travels only for a non-zero owner byte;
//! - `PLAYER_TRADE` action 0x40 from the server: the decoder writes its own
//!   target into the input instead of reading it (divergence D106), so the
//!   target is 0.
//!
//! [`SKIPPED`] lists the packets whose C++ decoder is undefined on its own
//! encoder's output; they have no decode line.

use game_sim::protocol::common::{FireComponent, FireWeapon, PlayerTrade, UpdateObject};
use game_sim::protocol::records::{CreateEquip, CreateHardpoint, CreateObject};
use game_sim::protocol::to_client::{
    CreateGuided, CreateLoot, CreateMine, CreateShip, CreateSolar, SendComm, SetGroupFeelings,
};
use game_sim::protocol::to_server::CreateNewChar;
use game_sim::protocol::{Body, ClientMessage, CommonMessage, ServerMessage};
use neuron_core::net::wire::quantise::{
    UNKNOWN_ARCHETYPE_MAX_HIT_POINTS, expand_fraction, expand_hull, expand_tenths, expand_unit,
    quantise_fraction, quantise_hull, quantise_tenths, quantise_unit,
};

use super::fill::Mode;

/// The packets without a decode line: `CREATENEWCHAR` reads the fields
/// after an empty name two bytes early (divergence D108), and
/// `SETGROUPFEELINGS` reads a relation count that is not sent (D109).
pub const SKIPPED: &[u32] = &[CreateNewChar::ID, SetGroupFeelings::ID];

/// A × 127 byte through `RpcServerExpand127` / `RpcUpdateExpand` and back.
fn unit(byte: u8) -> u8 {
    quantise_unit(expand_unit(byte))
}

fn units(bytes: [u8; 4]) -> [u8; 4] {
    bytes.map(unit)
}

/// A hull byte through `RpcServerExpandHull` and `RpcServerQuantizeHull`.
fn hull(byte: u8) -> u8 {
    let max = UNKNOWN_ARCHETYPE_MAX_HIT_POINTS;
    quantise_hull(expand_hull(byte, max), max)
}

/// A × 255 byte through `RpcServerExpand255` and back.
fn fraction(byte: u8) -> u8 {
    quantise_fraction(expand_fraction(byte))
}

/// The hardpoint `RpcServerDecodeCreateObjectEquip` stores, as the encoder
/// sends it again: a code's name (none for an unknown code), or the text
/// up to its first NUL looked up in the dictionary (none for an empty
/// text).
fn hardpoint(value: &CreateHardpoint) -> CreateHardpoint {
    match value {
        CreateHardpoint::None => CreateHardpoint::None,
        CreateHardpoint::Code(code) => CreateHardpoint::code_name(*code)
            .map_or(CreateHardpoint::None, |name| {
                CreateHardpoint::from_name(name.as_bytes())
            }),
        CreateHardpoint::Text(text) if text.is_empty() => CreateHardpoint::None,
        CreateHardpoint::Text(text) => {
            let name = text.split(|&b| b == 0).next().unwrap_or_default();
            match CreateHardpoint::from_name(name) {
                CreateHardpoint::Code(code) => CreateHardpoint::Code(code),
                _ => CreateHardpoint::Text(text.clone()),
            }
        }
    }
}

fn equip(record: &CreateEquip) -> CreateEquip {
    CreateEquip {
        health: record
            .health
            .and_then(|byte| CreateEquip::health_byte(expand_fraction(byte))),
        archetype: 0,
        hardpoint: hardpoint(&record.hardpoint),
        ..record.clone()
    }
}

/// `RpcServerCreateObjectDecode`, then `RpcServerCreateObjectEncode`.
fn create_object(object: &CreateObject) -> CreateObject {
    let mut out = object.clone();
    out.archetype = 0;
    out.orientation = units(object.orientation);
    out.hull = hull(object.hull);
    out.equipment = object.equipment.iter().map(equip).collect();
    for group in &mut out.collision_groups {
        group.health = fraction(group.health);
    }
    out
}

/// A channel 1 message after the C++ round trip.
pub fn common(message: &CommonMessage, mode: Mode) -> CommonMessage {
    match message {
        CommonMessage::UpdateObject(m) => CommonMessage::UpdateObject(UpdateObject {
            orientation: units(m.orientation),
            extra: unit(m.extra),
            ..m.clone()
        }),
        CommonMessage::FireWeapon(m) if mode != Mode::Single => {
            let [x, y, z] = m.target.map(FireComponent::value);
            CommonMessage::FireWeapon(FireWeapon {
                target: FireWeapon::target_from([x, y, z]),
                ..m.clone()
            })
        }
        CommonMessage::PlayerTrade(m) if mode == Mode::Server && m.action == 0x40 => {
            CommonMessage::PlayerTrade(PlayerTrade {
                target_player: 0,
                ..m.clone()
            })
        }
        other => other.clone(),
    }
}

/// A channel 3 message after the C++ round trip: no decoder converts.
pub fn client(message: &ClientMessage) -> ClientMessage {
    message.clone()
}

/// A channel 2 message after the C++ round trip.
pub fn server(message: &ServerMessage) -> ServerMessage {
    match message {
        ServerMessage::CreateShip(m) => {
            let mut out = CreateShip {
                object: create_object(&m.object),
                throttle: unit(m.throttle),
                attitude: unit(m.attitude),
                ..m.clone()
            };
            // `OwnerPlayer` holds the owner byte; the flag goes out for a
            // non-zero one, and the name blocks (empty) instead.
            if out.owner.as_ref().is_some_and(|owner| owner.player == 0) {
                out.owner = None;
                out.names = (Vec::new(), Vec::new());
            }
            ServerMessage::CreateShip(out)
        }
        ServerMessage::CreateSolar(m) => ServerMessage::CreateSolar(CreateSolar {
            object: create_object(&m.object),
            attitude: unit(m.attitude),
            ..m.clone()
        }),
        ServerMessage::CreateGuided(m) => ServerMessage::CreateGuided(CreateGuided {
            archetype: 0,
            orientation: units(m.orientation),
            ..m.clone()
        }),
        ServerMessage::CreateLoot(m) => {
            let mut out: CreateLoot = m.clone();
            for record in &mut out.loot {
                record.contents = 0;
                record.container = 0;
            }
            ServerMessage::CreateLoot(out)
        }
        ServerMessage::CreateMine(m) => ServerMessage::CreateMine(CreateMine {
            archetype: 0,
            ..m.clone()
        }),
        ServerMessage::SendComm(m) => ServerMessage::SendComm(SendComm {
            duration: quantise_tenths(expand_tenths(m.duration)),
            ..m.clone()
        }),
        other => other.clone(),
    }
}
