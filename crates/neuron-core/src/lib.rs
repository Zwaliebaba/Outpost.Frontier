//! Shared engine facilities for the client and server.

pub mod catalog;
pub mod diag;
pub mod hash;
pub mod id;
pub mod ini;
pub mod math;
pub mod msvcrt;
pub mod net;
pub mod num;
pub mod rng;
pub mod time;
pub mod utf;
pub mod util;
pub mod vfs;

/// Version of the shared engine.
pub const ENGINE_VERSION: &str = env!("CARGO_PKG_VERSION");
