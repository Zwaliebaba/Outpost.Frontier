//! Scene scripts (`.thn`), work package P4.14: the port of `SceneScript`
//! (`CppSource/SceneScript`). The chunk reader, evaluator, the parser's two
//! passes, curves and splines live in `neuron-core::scene_script`
//! (decision O44) and are re-exported here; the playback and the script
//! engine are this crate's.
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
//! tables into entity and event descriptors, and the run time the
//! descriptors drive: param curves ([`curve`], `ParamCurve.cpp`), motion
//! paths ([`spline`], `SplinePath.cpp` and `SplineMath.h`) and the scene
//! with its timeline ([`playback`], `SceneScriptScene.cpp` and the parts of
//! `SceneScript.cpp` that drive it), whose engine effects go through the
//! [`playback::EntityControls`] a host attaches to an entity. Saving a
//! scene (`SaveToFileSystem`) is [`playback::Player::save_to`]; the script
//! engine of `SceneScript.cpp` around the scene, with its COM context, its
//! load and save parsers and its file-system paths, is [`ScriptEngine`].
//!
//! The evaluator follows Lua 3.2.2, whose copyright notice is kept here as
//! its licence asks:
//!
//! ```text
//! Copyright (c) 1994-1999 TeCGraf, PUC-Rio.  All rights reserved.
//!
//! Permission is hereby granted, without written agreement and without license
//! or royalty fees, to use, copy, modify, and distribute this software and its
//! documentation for any purpose, including commercial applications, subject to
//! the following conditions:
//!
//!  * The above copyright notice and this permission notice shall appear in all
//!    copies or substantial portions of this software.
//!
//!  * The origin of this software must not be misrepresented; you must not
//!    claim that you wrote the original software. If you use this software in a
//!    product, an acknowledgment in the product documentation would be greatly
//!    appreciated (but it is not required).
//!
//!  * Altered source versions must be plainly marked as such, and must not be
//!    misrepresented as being the original software.
//!
//! The authors specifically disclaim any warranties, including, but not limited
//! to, the implied warranties of merchantability and fitness for a particular
//! purpose.  The software provided hereunder is on an "as is" basis, and the
//! authors have no obligation to provide maintenance, support, updates,
//! enhancements, or modifications.  In no event shall TeCGraf, PUC-Rio, or the
//! authors be held liable to any party for direct, indirect, special,
//! incidental, or consequential damages arising out of the use of this software
//! and its documentation.
//!
//! The Lua language and this implementation have been entirely designed and
//! written by Waldemar Celes, Roberto Ierusalimschy, and Luiz Henrique de
//! Figueiredo at TeCGraf, PUC-Rio.
//! ```
//!
//! This is an altered version: a Rust evaluator for a subset of the
//! virtual machine, not the original C source.

pub use neuron_core::scene_script::{
    build, chunk, curve, entity, event, load, number, spline, spline_math, table, vm,
};
pub mod playback;
mod script_engine;

pub use neuron_core::scene_script::{
    BuildError, BuildState, LoadError, LoadSummary, Native, NativeKind, SCENE_CONSTANTS, Scene,
    ScriptError, Step, Table, TableError, TableId, Value, Vm,
};
pub use script_engine::{
    CREATE_ALWAYS, ComContext, DefaultSaveParser, FileSystemFactory, LoadParser, SaveParser,
    ScriptEngine, ScriptModule, host_file_systems,
};
