//! Outpost:Frontier server startup and authoritative game rules.
#![forbid(unsafe_code)]

pub mod character_file;
pub mod events;

use std::io::{self, Write};

/// Start the server application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier server (Neuron {})",
        neuron_server::ENGINE_VERSION
    )
}
