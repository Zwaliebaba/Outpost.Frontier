//! The Lua 3.2 compiler of `cargo xtask thn-decompile` (`xtask/src/thn`),
//! compiled into the tests: the port loads compiled chunks only (decision
//! O18, divergence D86), and a saved scene is Lua source, so a test that
//! reloads one compiles it first, as O18 says a modded script must be.
//! The compiler accepts the language the scene scripts use, which is the
//! language the writer writes.

#[path = "../../../../xtask/src/thn/chunk.rs"]
mod chunk;
#[path = "../../../../xtask/src/thn/compile.rs"]
mod compile;
#[path = "../../../../xtask/src/thn/number.rs"]
mod number;

/// The compiled chunk of `source`, or the compiler's error.
pub fn compile(source: &[u8]) -> Result<Vec<u8>, String> {
    compile::compile(source).map(|chunk| chunk.write())
}
