//! The first pass of the scene-script parser, `FSceneScriptLuaLoadParser::
//! Load` in `CppSource/SceneScript/SceneScriptLua.cpp`: run the script,
//! then count its entities and events and read its duration.

use std::fmt;

use crate::num;

use super::table::Value;
use super::vm::{ScriptError, Vm};

/// What `Load` reports about a script.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LoadSummary {
    /// Entries of the global `entities` whose value is a table.
    pub entities: u32,
    /// Entries of the global `events` whose value is a table; 0 when
    /// `events` is not a table.
    pub events: u32,
    /// The global `duration` in milliseconds.
    pub duration_ms: u32,
}

/// Why `Load` returned an error.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LoadError {
    /// The file is empty ("LUA unable to read file").
    Empty,
    /// `lua_dobuffer` failed ("LUA unable to parse file").
    Script(ScriptError),
    /// The global `entities` is not a table.
    NoEntities,
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => f.write_str("the scene script is empty"),
            Self::Script(e) => write!(f, "the scene script does not run: {e}"),
            Self::NoEntities => f.write_str("the scene script defines no entity table"),
        }
    }
}

impl std::error::Error for LoadError {}

/// `lua_getnumber`: a number, or a string that converts to one; else 0.
pub(super) fn get_number(v: &Value) -> f64 {
    match v {
        Value::Number(x) => *x,
        Value::String(s) => super::number::to_number(s).unwrap_or(0.0),
        _ => 0.0,
    }
}

/// Runs `bytes` in `vm` and summarises the result as `Load` does.
pub fn load(vm: &mut Vm, bytes: &[u8]) -> Result<LoadSummary, LoadError> {
    if bytes.is_empty() {
        return Err(LoadError::Empty);
    }
    vm.run(bytes).map_err(LoadError::Script)?;
    let count = |name: &str| -> Option<u32> {
        let id = vm.global(name).as_table()?;
        let n = vm
            .table(id)
            .entries()
            .filter(|(_, v)| v.as_table().is_some())
            .count();
        Some(u32::try_from(n).expect("a table has fewer than 2^32 entries"))
    };
    let entities = count("entities").ok_or(LoadError::NoEntities)?;
    let events = count("events").unwrap_or(0);
    // `LuaMillisecondsTobaselineInt`: the product on the 53-bit stack,
    // chopped to a qword, the low word kept.
    let duration_ms =
        num::trunc_to_i32_via_i64(get_number(vm.global("duration")) * 1000.0).cast_unsigned();
    Ok(LoadSummary {
        entities,
        events,
        duration_ms,
    })
}
