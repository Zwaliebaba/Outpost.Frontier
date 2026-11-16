//! Wire primitives, framing and transports (work package P1.9, decisions
//! T16 and T17).
//!
//! - [`wire`]: the field encodings and quantisers of the C++ codecs,
//!   bit-exact, and the [`wire::WireMessage`] trait the message catalog in
//!   `game-sim` implements.
//! - [`frame`]: the framing that replaces DirectPlay's message boundaries
//!   and the implicit compression, and the connection handshake.
//! - [`transport`]: the [`transport::Transport`] trait the QUIC transports
//!   implement, and the loopback transport of single player.

pub mod frame;
pub mod transport;
pub mod wire;
