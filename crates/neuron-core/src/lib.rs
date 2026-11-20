//! Shared engine facilities for the client and server.

pub mod account;
pub mod anim;
pub mod catalog;
pub mod diag;
pub mod fx_data;
pub mod hash;
pub mod id;
pub mod image;
pub mod ini;
pub mod material_data;
pub mod math;
pub mod mesh_data;
pub mod msvcrt;
pub mod net;
pub mod num;
pub mod physics;
pub mod rng;
pub mod scene;
pub mod sur;
pub mod time;
pub mod utf;
pub mod util;
pub mod vfs;

/// Version of the shared engine.
pub const ENGINE_VERSION: &str = env!("CARGO_PKG_VERSION");
