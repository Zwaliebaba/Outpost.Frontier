//! Scene scripts (`.thn`), the platform-free part of work package P4.14:
//! the chunk reader and evaluator of `CppSource/SceneScript` and the
//! parser's two passes (decision O44).
//!
//! A scene script is a compiled Lua 3.2 chunk that assigns three globals,
//! `entities`, `events` and `duration`, from table constructors, and is
//! never called again. The reference runs it on the Lua 3.2 interpreter and
//! then walks the tables. The port reads the chunk and evaluates the 15
//! opcodes the shipped scripts use (decisions T8 and O18, design/facts.md
//! F15); anything else is rejected (divergence D86).
//!
//! The module holds the evaluator ([`Vm`]), the parser's first pass
//! ([`load()`]) and its second ([`build()`]), which turns the evaluated
//! tables into entity and event descriptors, and the descriptors' own
//! arithmetic: param curves ([`curve`], `ParamCurve.cpp`) and motion paths
//! ([`spline`], `SplinePath.cpp` and `SplineMath.h`). The scene's playback
//! and the script engine around it stay in `neuron-client::scene_script`;
//! the server reads a room's set script through this module
//! (`RoomData::read_set_script`).

pub mod build;
pub mod chunk;
pub mod curve;
pub mod entity;
pub mod event;
pub mod load;
pub mod number;
pub mod spline;
pub mod spline_math;
pub mod table;
pub mod vm;

pub use build::{BuildError, BuildState, Scene, Step, build};
pub use load::{LoadError, LoadSummary, load};
pub use table::{Native, NativeKind, Table, TableError, TableId, Value};
pub use vm::{SCENE_CONSTANTS, ScriptError, Vm};
