//! Outpost:Frontier server startup and authoritative game rules.
#![forbid(unsafe_code)]

pub mod api;
pub mod bases;
pub mod character_file;
pub mod chat;
pub mod cheats;
pub mod client;
pub mod dock;
pub mod events;
pub mod fuses;
pub mod group;
pub mod login;
pub mod loot;
pub mod munitions;
pub mod objects;
pub mod player;
pub mod save_writer;
pub mod selection;
pub mod server;
pub mod ship;
pub mod world;

use std::io::{self, Write};

/// Start the server application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier server (Neuron {})",
        neuron_server::ENGINE_VERSION
    )
}
