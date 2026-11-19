//! Engine facilities for client presentation, input, and connectivity.

pub use neuron_core::ENGINE_VERSION;

pub mod audio;
pub mod engine;
pub mod input;
pub mod media;
#[cfg(windows)]
pub mod platform;
pub mod render;
pub mod scene_script;
pub mod text;
pub mod text_input;
pub mod ui;
pub mod user_state;
pub mod window;
