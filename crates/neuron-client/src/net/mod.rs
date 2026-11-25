//! The client side of the QUIC transport (work package P3.8, decisions
//! O6, T16, T17): `CDPClient` and the connection handling of
//! `RemoteServer`.
//!
//! - [`QuicClient`] implements [`neuron_core::net::transport::Transport`]
//!   for the game thread. One I/O thread owns a current-thread tokio
//!   runtime and the quinn endpoint; `send` and `poll` only touch queues
//!   and never block.
//! - A connection opens one bidirectional stream and sends a `Hello`
//!   with the protocol version, the build, the account and the password;
//!   the game stream starts after the server's `Welcome`.
//! - [`verify`]: the server's self-signed certificate is pinned by its
//!   fingerprint ([`neuron_core::net::pin`]); the handshake signature is
//!   verified against it.
//! - [`StatusQuery`]: a handshake that asks for the server's status only,
//!   for the address book's rows.

mod client;
mod io;
pub mod verify;

pub use client::{
    ClientTiming, ConnectConfig, MESSAGE_LIMIT, QuicClient, SERVER_CONNECTION, StatusQuery,
    StatusReply,
};
