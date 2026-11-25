//! The message protocol client and server share (work package P2.2,
//! decision T17): the 162 packet types of `RemoteCommon/` and the `Common`
//! encodings they carry.
//!
//! - [`marshal`]: `MarshalBuffer`, the length-prefixed scalar and string
//!   encoding of `Common` (`common_marshalbuffer.cpp`), with the equipment
//!   and goods lists.
//! - [`fmtstr`]: `FmtStr`, the formatted-string tree, and its flattened
//!   form (`common_fmtstr.cpp`).
//! - [`costume`]: `Costume` and the forms it takes on the wire.
//! - [`rdl`]: binary rich text, the record stream of `BinaryRDLReader` and
//!   `BinaryRDLWriter` (`common_rdlio.cpp`). The RDLX markup path is the
//!   client's (P4.12).
//! - [`common`], [`to_client`], [`to_server`]: the message catalog by
//!   channel, as `include/network.h` groups it: channel 1 travels both ways
//!   (21 opcodes, 22 C++ classes counting the `CYCLOPSMESSAGE` alias), 2
//!   from server to client (81) and 3 from client to server (59).
//! - [`records`]: the records several messages share (equipment, collision
//!   groups, damage, the create-object block).
//!
//! # Wire values
//!
//! The message types hold what travels: a quantised field is its byte, an
//! archetype that travels as a small ID is that ID, a count is the length
//! of its list. Decoding and re-encoding a message therefore reproduces its
//! bytes. The helpers that turn engine values into wire values are the
//! quantisers of [`neuron_core::net::wire::quantise`] and
//! [`quantise_orientation`]; the archetype tables that map small IDs belong
//! to `game-sim::data` (P2.1).
//!
//! Every field is little-endian with the C++ width. Floats the C++ moves
//! through the x87 stack on encode are written with
//! [`WireWriter::put_f32_x87`] (a signalling NaN goes out quiet); every
//! decoder copies the bits.
//!
//! # Single player
//!
//! Some C++ codecs ask `SinglePlayer()` or `IsMPServer()` and write more or
//! fewer fields: `FIREWEAPON`, `PLAYER_TRADE`, `CREATESHIP`, `CREATESOLAR`,
//! `CREATELOOT`, `CREATEMINE` and `SETREPUTATION`. Those are the
//! single-player payload forms of `rpclocal` (`rpc_payload_ch*.cpp`), which
//! leave out what the shared world already holds (decision O21,
//! [`adr/0002`](../../../design/adr/0002-single-player-world.md)). A
//! [`WireContext`] selects the form; each message documents its fields that
//! travel in one form only.
//!
//! `rpclocal` itself never encodes: it hands its payload structs over, and
//! those keep engine values where the wire quantises (the throttle, hit
//! points and orientations of the create messages, `UPDATEOBJECT`'s
//! orientation). The typed single-player path of the loopback transport
//! (P3) has to carry those values; these types hold wire values only.
//!
//! The channel enums are not [`neuron_core::net::wire::WireMessage`]s:
//! their codecs need a [`WireContext`], which that trait has no place for.
//! The QUIC transports carry [`MultiplayerToClient`] and
//! [`MultiplayerToServer`], which bind the multiplayer contexts (P3.8).
//!
//! # Differences from the C++
//!
//! The decoders check every length against the input and fail with a
//! [`WireError`] where the C++ reads uninitialised memory, writes through a
//! null pointer or past an array, or reports success over a failed field.
//! The encoders refuse a count that does not fit its prefix where the C++
//! writes the truncated count and every element. The register in
//! `design/divergences.md` lists each case (D97 to D109).

#[macro_use]
mod field;

pub mod base_props;
pub mod common;
pub mod costume;
pub mod fmtstr;
pub mod marshal;
pub mod rdl;
pub mod records;
pub mod to_client;
pub mod to_server;

pub use neuron_core::net::wire::{MessageHeader, WireError, WireReader, WireWriter};

use neuron_core::net::wire::quantise::quantise_unit;

pub use common::CommonMessage;
pub use to_client::ServerMessage;
pub use to_server::ClientMessage;

/// The game's packed version, `((minor | major << 8) << 16) | build`
/// (`GServerVersion`, `OutpostServer.cpp:422-431`): 1.0 build 11, the C++'s
/// value when the executable has no version resource. The QUIC handshake
/// refuses a peer of another build (D894).
pub const GAME_BUILD: u32 = 0x0100_000b;

/// The C++ encode buffer: 10 240 bytes in both directions
/// (`RemoteClient_transport.cpp`, `RemoteServer_main.cpp`).
pub const ENCODE_LIMIT: usize = 0x2800;

/// The server's second attempt for a message that does not fit
/// [`ENCODE_LIMIT`]: 102 400 bytes.
pub const ENCODE_RETRY_LIMIT: usize = 0x19000;

/// Which way a message travels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Direction {
    /// From the server to a client (channels 1 and 2).
    ToClient,
    /// From a client to the server (channels 1 and 3).
    ToServer,
}

/// What the C++ codecs read from `Common`'s globals: `SinglePlayer()` and,
/// through the direction, `IsMPServer()`.
///
/// `IsMPServer()` is true in the dedicated server's process and false in a
/// client's and in single player, so on the encoding side it means "a
/// multiplayer server sending to a client" and on the decoding side "a
/// multiplayer server receiving from a client".
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct WireContext {
    /// `SinglePlayer()`: selects the single-player payload forms.
    pub single_player: bool,
    /// The direction of the message.
    pub direction: Direction,
}

impl WireContext {
    /// A multiplayer server's messages to a client.
    pub const SERVER: Self = Self {
        single_player: false,
        direction: Direction::ToClient,
    };
    /// A multiplayer client's messages to the server.
    pub const CLIENT: Self = Self {
        single_player: false,
        direction: Direction::ToServer,
    };
    /// Single player, the local server's messages to the client.
    pub const SINGLE_PLAYER_SERVER: Self = Self {
        single_player: true,
        direction: Direction::ToClient,
    };
    /// Single player, the client's messages to the local server.
    pub const SINGLE_PLAYER_CLIENT: Self = Self {
        single_player: true,
        direction: Direction::ToServer,
    };

    /// `IsMPServer()` on the encoding side: a multiplayer server sends.
    #[must_use]
    pub const fn encoder_is_mp_server(&self) -> bool {
        !self.single_player && matches!(self.direction, Direction::ToClient)
    }
}

/// The C++ packet IDs: `channel << 16 | opcode` (`EFlPacket`).
pub type PacketId = u32;

/// The header of a packet ID.
#[must_use]
pub const fn header_of(id: PacketId) -> MessageHeader {
    let [opcode, _, channel, _] = id.to_le_bytes();
    MessageHeader::new(channel, opcode)
}

/// One message body with its packet ID; the channel enums dispatch on it.
pub trait Body: Sized {
    /// The packet ID.
    const ID: PacketId;
    /// The C++ name (`FLPACKET_*`).
    const NAME: &'static str;

    /// Writes the fields after the header.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the writer's limit is passed, or
    /// [`WireError::LengthOutOfRange`] / [`WireError::Invalid`] for a value
    /// the encoding cannot carry.
    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError>;

    /// Reads the fields after the header.
    ///
    /// # Errors
    /// Any [`WireError`] for input that is not such a body.
    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError>;

    /// The header and the fields.
    ///
    /// # Errors
    /// As [`Body::encode_body`].
    fn encode(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_header(header_of(Self::ID))?;
        self.encode_body(ctx, writer)
    }

    /// The message's bytes, header included, without a size limit.
    ///
    /// # Errors
    /// As [`Body::encode_body`].
    fn to_wire(&self, ctx: &WireContext) -> Result<Vec<u8>, WireError> {
        let mut writer = WireWriter::new();
        self.encode(ctx, &mut writer)?;
        Ok(writer.into_bytes())
    }
}

/// The four bytes `CREATESHIP`, `CREATESOLAR`, `CREATEGUIDED` and
/// `UPDATEOBJECT` send for an orientation `[w, x, y, z]`: `x`, `y`, `z`,
/// `w`, each × 127 truncated (`RpcServerQuantize127`, `RpcUpdateQuantize`).
#[must_use]
pub fn quantise_orientation(wxyz: [f32; 4]) -> [u8; 4] {
    let [w, x, y, z] = wxyz;
    [
        quantise_unit(x),
        quantise_unit(y),
        quantise_unit(z),
        quantise_unit(w),
    ]
}

/// The orientation `[w, x, y, z]` four wire bytes `x`, `y`, `z`, `w` stand
/// for (`RpcServerExpand127`).
#[must_use]
pub fn expand_orientation(xyzw: [u8; 4]) -> [f32; 4] {
    use neuron_core::net::wire::quantise::expand_unit;
    let [x, y, z, w] = xyzw;
    [
        expand_unit(w),
        expand_unit(x),
        expand_unit(y),
        expand_unit(z),
    ]
}

/// A message from the server to a client: channel 1 or 2.
#[derive(Clone, Debug, PartialEq)]
pub enum ToClient {
    /// Channel 1.
    Common(CommonMessage),
    /// Channel 2.
    Server(ServerMessage),
}

/// A message from a client to the server: channel 1 or 3.
#[derive(Clone, Debug, PartialEq)]
pub enum ToServer {
    /// Channel 1.
    Common(CommonMessage),
    /// Channel 3.
    Client(ClientMessage),
}

impl ToClient {
    /// Writes the message.
    ///
    /// # Errors
    /// As [`Body::encode_body`].
    pub fn encode(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::Common(message) => message.encode(ctx, writer),
            Self::Server(message) => message.encode(ctx, writer),
        }
    }

    /// Reads one message, dispatching on the header's channel.
    ///
    /// # Errors
    /// [`WireError::UnknownMessage`] for a header of another channel or an
    /// opcode the catalog does not have, otherwise as [`Body::decode_body`].
    pub fn decode(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let header = reader.clone().get_header()?;
        match header.channel {
            1 => CommonMessage::decode(ctx, reader).map(Self::Common),
            2 => ServerMessage::decode(ctx, reader).map(Self::Server),
            _ => Err(WireError::UnknownMessage(header)),
        }
    }
}

impl ToServer {
    /// Writes the message.
    ///
    /// # Errors
    /// As [`Body::encode_body`].
    pub fn encode(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::Common(message) => message.encode(ctx, writer),
            Self::Client(message) => message.encode(ctx, writer),
        }
    }

    /// Reads one message, dispatching on the header's channel.
    ///
    /// # Errors
    /// [`WireError::UnknownMessage`] for a header of another channel or an
    /// opcode the catalog does not have, otherwise as [`Body::decode_body`].
    pub fn decode(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let header = reader.clone().get_header()?;
        match header.channel {
            1 => CommonMessage::decode(ctx, reader).map(Self::Common),
            3 => ClientMessage::decode(ctx, reader).map(Self::Client),
            _ => Err(WireError::UnknownMessage(header)),
        }
    }
}

/// A [`ToClient`] as the QUIC transports carry it: encoded and decoded in
/// the multiplayer server's form ([`WireContext::SERVER`]) at both ends.
#[derive(Clone, Debug, PartialEq)]
pub struct MultiplayerToClient(pub ToClient);

impl From<ToClient> for MultiplayerToClient {
    fn from(message: ToClient) -> Self {
        Self(message)
    }
}

impl neuron_core::net::wire::WireMessage for MultiplayerToClient {
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        self.0.encode(&WireContext::SERVER, writer)
    }

    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        ToClient::decode(&WireContext::SERVER, reader).map(Self)
    }
}

/// A [`ToServer`] as the QUIC transports carry it: the multiplayer
/// client's form ([`WireContext::CLIENT`]) at both ends.
#[derive(Clone, Debug, PartialEq)]
pub struct MultiplayerToServer(pub ToServer);

impl From<ToServer> for MultiplayerToServer {
    fn from(message: ToServer) -> Self {
        Self(message)
    }
}

impl From<MultiplayerToServer> for ToServer {
    fn from(message: MultiplayerToServer) -> Self {
        message.0
    }
}

impl neuron_core::net::wire::WireMessage for MultiplayerToServer {
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        self.0.encode(&WireContext::CLIENT, writer)
    }

    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        ToServer::decode(&WireContext::CLIENT, reader).map(Self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packet_ids_split_into_headers() {
        assert_eq!(header_of(0x2004f), MessageHeader::new(2, 0x4f));
        assert_eq!(header_of(0x10001).to_word(), 0x0101);
    }

    #[test]
    fn orientation_bytes_are_x_y_z_w() {
        assert_eq!(
            quantise_orientation([1.0, 0.0, -1.0, 0.5]),
            [0, 0x81, 63, 127]
        );
        assert_eq!(expand_orientation([0, 0, 0, 127]), [1.0, 0.0, 0.0, 0.0]);
    }

    #[test]
    fn multiplayer_adapters_bind_their_context() {
        use neuron_core::net::wire::WireMessage;
        let message = MultiplayerToClient::from(ToClient::Server(
            to_client::ConnectResponse { value: 3 }.into(),
        ));
        let bytes = message.to_wire().unwrap();
        assert_eq!(bytes, [0x01, 0x02, 3, 0, 0, 0]);
        assert_eq!(MultiplayerToClient::from_wire(&bytes).unwrap(), message);
        assert!(MultiplayerToServer::from_wire(&bytes).is_err());
        let message = MultiplayerToServer::from(ToServer::Common(
            common::RequestPlayerStats::default().into(),
        ));
        let bytes = message.to_wire().unwrap();
        assert_eq!(MultiplayerToServer::from_wire(&bytes).unwrap(), message);
    }

    #[test]
    fn context_mp_server() {
        assert!(WireContext::SERVER.encoder_is_mp_server());
        assert!(!WireContext::CLIENT.encoder_is_mp_server());
        assert!(!WireContext::SINGLE_PLAYER_SERVER.encoder_is_mp_server());
    }
}
