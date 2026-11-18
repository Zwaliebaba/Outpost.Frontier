//! Engine facilities for client presentation, input, and connectivity.

pub use neuron_core::ENGINE_VERSION;

#[cfg(windows)]
pub mod platform;
pub mod render;
pub mod scene_script;
pub mod text;
