//! Shared engine facilities for the client and server.

pub mod diag;
pub mod hash;
pub mod id;
pub mod ini;
pub mod msvcrt;
pub mod num;
pub mod rng;
pub mod time;
pub mod util;

/// Version of the shared engine.
pub const ENGINE_VERSION: &str = env!("CARGO_PKG_VERSION");
