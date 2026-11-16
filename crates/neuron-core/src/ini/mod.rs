//! INI formats (work package P1.2): the engine configuration parser and the
//! game INI reader.

mod engine;
mod number;
mod reader;

pub use engine::{
    EngineEntry, EngineIni, EngineIniError, EngineSection, IncludeLoader, LOCAL_BUFFER_NAME,
    NoIncludes,
};
pub use number::{ParsedNumber, is_number, parse_number, string_index, value_num};
pub use reader::{IniReader, IniReport, IniState};
