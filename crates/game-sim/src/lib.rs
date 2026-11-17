//! Outpost:Frontier simulation shared by the client and the server: game
//! data, the space object model, equipment, flight AI and the message
//! protocol.
//!
//! - [`protocol`]: the message catalog and the `Common` encodings it
//!   carries (work package P2.2).
#![forbid(unsafe_code)]

pub mod protocol;

pub use neuron_core::ENGINE_VERSION;
