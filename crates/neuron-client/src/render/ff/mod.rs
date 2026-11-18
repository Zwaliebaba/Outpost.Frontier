//! The platform-free model of the fixed-function device (decision record
//! 0004, point 6; work package P4.3): everything the renderer does that is
//! not GPU work, built and tested on every host.
//!
//! So far: the render, stage and pipeline states with their defaults
//! ([`state`], [`defaults`], [`model`]), vertex formats ([`fvf`]) and
//! primitive counting and conversion ([`geometry`]). Transforms, lights,
//! materials, resources, the draw packet and the recording backend follow.

pub mod arena;
pub mod buffers;
pub mod defaults;
pub mod draw;
pub mod fvf;
pub mod geometry;
pub mod lights;
pub mod model;
pub mod state;
pub mod textures;
pub mod transforms;
