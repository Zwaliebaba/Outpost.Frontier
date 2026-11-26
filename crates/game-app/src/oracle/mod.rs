//! The port's `+oracle` mode (decision O53 (2), record 0009 point 6): the
//! C++ client's script language and JSON Lines files
//! ([reference-build.md](../../../design/reference-build.md) section 8),
//! written at the same hooks so that CI compares the two clients' files.

pub mod iclient;
pub mod json;
pub mod keymap;
pub mod layout;
pub mod script;
