//! Outpost:Frontier client startup and game-specific presentation.

use std::io::{self, Write};

/// Start the client application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier client (Neuron {})",
        neuron_client::ENGINE_VERSION
    )
}
