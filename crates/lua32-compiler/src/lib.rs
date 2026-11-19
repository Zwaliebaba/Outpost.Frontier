//! The Lua 3.2 subset compiler of the precompiled `.thn` scene scripts
//! (design/facts.md F15): the chunk format, the compiler and the number
//! text the shipped chunks store.
//!
//! The port loads compiled chunks only (design/decisions.md O18), so a
//! Lua source that should reach the engine is compiled first. Two users
//! need it: `cargo xtask thn-decompile`, which proves each decompiled
//! script compiles back to its chunk byte for byte, and the neuron-client
//! test that saves every shipped scene to source and reloads it. It is a
//! development tool, not part of either executable.
//!
//! * [`compile`]: source in the subset the scene scripts use to the
//!   [`Chunk`] the reference compiler makes from it.
//! * [`Chunk`]: the precompiled chunk, read with [`Chunk::read`] and
//!   written with [`Chunk::write`].
//! * [`source_numeral`]: the numeral to write in source for a stored
//!   number constant.
#![forbid(unsafe_code)]

mod chunk;
mod compile;
mod number;

pub use chunk::{Chunk, ChunkError, Constant, HEADER, op};
pub use compile::{CompileError, compile};
pub use number::source_numeral;
