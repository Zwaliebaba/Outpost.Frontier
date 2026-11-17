//! Outpost:Frontier simulation shared by the client and the server: game
//! data, the space object model, equipment, flight AI and the message
//! protocol.
//!
//! * [`data`]: the game databases of `Common` (work package P2.1):
//!   constants, the `[Data]` list, goods, factions, the universe with its
//!   bases and rooms, zones, asteroid ids, loadouts, archetypes, fuses, the
//!   spatial partition and the string cache.
#![forbid(unsafe_code)]

pub mod data;

pub use neuron_core::ENGINE_VERSION;
