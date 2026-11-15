//! Outpost:Frontier server startup and authoritative game rules.

use std::io::{self, Write};

/// Start the server application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier server (Neuron {})",
        neuron_server::ENGINE_VERSION
    )
}
