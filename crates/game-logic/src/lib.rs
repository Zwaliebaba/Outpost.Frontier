//! Outpost:Frontier server startup and authoritative game rules.
#![forbid(unsafe_code)]

pub mod bases;
pub mod character_file;
pub mod chat;
pub mod cheats;
pub mod client;
pub mod events;
pub mod group;
pub mod login;
pub mod player;
pub mod save_writer;
pub mod selection;
pub mod server;

use std::io::{self, Write};

/// Start the server application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier server (Neuron {})",
        neuron_server::ENGINE_VERSION
    )
}
