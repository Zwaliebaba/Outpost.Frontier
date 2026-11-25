//! The server side of the QUIC transport (work package P3.8, decisions
//! O6, T16, T17): `CDPServer`, `CDPClientProxy` and the connection
//! handling of `RemoteClient`.
//!
//! - [`QuicServer`] implements [`neuron_core::net::transport::Transport`]
//!   for the server thread. One I/O thread owns a current-thread tokio
//!   runtime and the quinn endpoint; [`Transport::send`] and
//!   [`Transport::poll`] only touch queues and never block.
//! - Each client opens one bidirectional stream and sends a
//!   [`Hello`](neuron_core::net::frame::Hello). The host answers a query
//!   with its status, refuses a join of another protocol version or
//!   build, with a wrong password or beyond the player limit, and
//!   otherwise admits it in the lowest free slot with a `Welcome`.
//! - [`governor`]: the batching of `CDPClientProxy::SendMsgs` and the
//!   saturation of `UpdateQueueHistory`, which
//!   [`Transport::link_saturation`] reports.
//! - [`identity`]: the self-signed certificate clients pin.
//!
//! The backlog the governor samples is what the game has sent and QUIC
//! has not taken. QUIC may hold two congestion windows (the send window
//! follows the congestion window), so the backlog grows when the link
//! carries less than the game sends, as DirectPlay's send queue did.
//!
//! [`Transport::send`]: neuron_core::net::transport::Transport::send
//! [`Transport::poll`]: neuron_core::net::transport::Transport::poll
//! [`Transport::link_saturation`]: neuron_core::net::transport::Transport::link_saturation

pub mod governor;
mod host;
pub mod identity;
mod io;

pub use host::{
    DEFAULT_MAX_PLAYERS, HostConfig, HostError, HostSettings, HostTiming, MAX_CLIENTS, PeerInfo,
    QuicServer,
};
pub use identity::{IdentityError, ServerIdentity};
