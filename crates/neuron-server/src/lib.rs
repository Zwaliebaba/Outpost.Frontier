//! Engine facilities for server hosting, sessions, and persistence.
//!
//! - [`controller`]: the controller registry, heartbeats, timers and the
//!   synchronous, depth-first dispatch of decision record 0006 (P3.1).
//! - [`save`]: the `FLS1` save container and the readers' magic test (P3.1).
//! - [`account`]: the account CRC and names, the accounts root, the `name`
//!   file and flags, the folder scan and the server's `accounts.cfg` (P3.1).
//! - [`session`]: the session slots, addressed by 1-based ids (P3.1).
//! - [`tick`]: the server frame's order and the server clock (P3.1).
//! - [`net`]: the QUIC host, its handshake, batching and saturation
//!   governor, and the server's certificate (P3.8).
//! - [`log`]: the dedicated server's log, `ServerLogf` (P3.9).
//! - [`admin_files`]: `motd.dat` and the old `OutpostServer.cfg` (P3.9).
//! - [`machine`]: the machine queries and generator of a first start's
//!   `accounts.cfg` without platform calls (P3.9).
#![forbid(unsafe_code)]

pub mod account;
pub mod admin_files;
pub mod chat_space;
pub mod controller;
pub mod log;
pub mod machine;
pub mod net;
pub mod registry;
pub mod save;
pub mod session;
pub mod tick;

pub use neuron_core::ENGINE_VERSION;
