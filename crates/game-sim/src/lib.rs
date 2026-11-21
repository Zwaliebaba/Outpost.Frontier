//! Outpost:Frontier simulation shared by the client and the server: game
//! data, the space object model, equipment, flight AI and the message
//! protocol.
//!
//! * [`data`]: the game databases of `Common` (work package P2.1):
//!   constants, the `[Data]` list, goods, factions, the universe with its
//!   bases and rooms, zones, asteroid ids, loadouts, archetypes, fuses, the
//!   spatial partition and the string cache.
//! * [`protocol`]: the message catalog and the `Common` encodings it
//!   carries (work package P2.2).
//! * [`object`]: the space object model, the world with its arena,
//!   handles, update loop and damage (work package P2.4).
//! * [`equip`]: equipment and the flight controllers (work package P2.4).
//! * [`ai`]: the flight AI (work package P2.5); so far the parts that need
//!   no object world and the world interface the behaviours will use.
//! * [`scenario`]: the oracle scenarios run through the object world and
//!   judged against the oracle's records (decision O45), with any server
//!   hooks.
#![forbid(unsafe_code)]

pub mod ai;
pub mod data;
pub mod equip;
pub mod object;
pub mod protocol;
pub mod scenario;

pub use neuron_core::ENGINE_VERSION;
