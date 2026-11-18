//! Outpost:Frontier client startup and game-specific presentation.
#![forbid(unsafe_code)]

use std::io::{self, Write};

pub mod account_keys;
pub mod login_history;
pub mod profile;
pub mod table_layouts;
pub mod timing;

/// Start the client application.
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier client (Neuron {})",
        neuron_client::ENGINE_VERSION
    )
}
