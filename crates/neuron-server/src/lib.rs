//! Engine facilities for server hosting, sessions, and persistence.
//!
//! - [`controller`]: the controller registry, heartbeats, timers and the
//!   synchronous, depth-first dispatch of decision record 0006 (P3.1).
//! - [`save`]: the `FLS1` save container and the readers' magic test (P3.1).
#![forbid(unsafe_code)]

pub mod controller;
pub mod save;

pub use neuron_core::ENGINE_VERSION;
