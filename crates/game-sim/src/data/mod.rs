//! The game databases of `Common` (work package P2.1).
//!
//! Each module ports one group of loaders from `CppSource/Common/`. The C++
//! keeps every database in a process global filled by `DataConfig::startup`;
//! here [`GameData`](config::GameData) owns them and
//! [`GameData::load`](config::GameData::load) runs the loaders in the order
//! of the `[Data]` list, as the C++ does. Loaders read files through a
//! [`DataSource`], the C++ `NeuronRuntime::Data` file system.
//!
//! Engine handles (meshes, animation scripts, collision surfaces) are not
//! part of the data: they are made when an object needs them (decision
//! record 0001, point 7).

pub mod archetype;
pub mod asteroid;
pub mod base;
pub mod config;
pub mod constants;
pub mod equip_desc;
pub mod goods;
pub mod loadout;
pub mod reputation;
pub mod string_cache;
pub mod universe;
pub mod zone;

use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

/// Where loaders read their files: `NeuronRuntime::Data` with
/// `GetFileSysPath`. A relative path is relative to the data root; an
/// absolute one (a leading `\`, or a drive letter) is used as it is.
pub trait DataSource {
    /// The bytes of the file `path` names, or `None` when it cannot be
    /// read. `path` ends at its first NUL byte.
    fn read(&self, path: &[u8]) -> Option<Vec<u8>>;
}

impl DataSource for Host {
    /// Reads through the host file system with the data root as the
    /// current directory; names are matched without regard to letter case
    /// where the host is case-sensitive.
    fn read(&self, path: &[u8]) -> Option<Vec<u8>> {
        self.read_file(path).ok().map(|file| file.data)
    }
}

/// `INI_Reader::open(GetFileSysPath(path), false)`: a reader on the file, or
/// `None` when it is missing or empty (an empty file cannot be mapped).
pub fn open_ini(source: &dyn DataSource, path: &[u8]) -> Option<IniReader> {
    let contents = source.read(path)?;
    let mut reader = IniReader::new();
    reader
        .open(neuron_core::hash::c_str(path), contents)
        .then_some(reader)
}

/// A warning or error the C++ writes through `FDUMP` while loading. The
/// text is the C++ message without its source-file prefix.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoadReport {
    /// The C++ source the message names (`Common\Constants.cpp` and so on).
    pub source: &'static str,
    /// The line number the message names.
    pub line: u32,
    /// The message.
    pub text: String,
}

/// The reports of a load, in the order the C++ writes them.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Reports(pub Vec<LoadReport>);

impl Reports {
    /// Adds a report.
    pub fn push(&mut self, source: &'static str, line: u32, text: impl Into<String>) {
        self.0.push(LoadReport {
            source,
            line,
            text: text.into(),
        });
    }
}

/// Bytes as text for a message, invalid UTF-8 replaced.
pub(crate) fn lossy(bytes: &[u8]) -> String {
    String::from_utf8_lossy(neuron_core::hash::c_str(bytes)).into_owned()
}

/// `!(a < b)` as the x87 compare tests it: also true when unordered (NaN).
pub(crate) fn not_below(a: f64, b: f64) -> bool {
    a.partial_cmp(&b) != Some(core::cmp::Ordering::Less)
}

/// `!(a > b)`, true when unordered.
pub(crate) fn not_above(a: f64, b: f64) -> bool {
    a.partial_cmp(&b) != Some(core::cmp::Ordering::Greater)
}

/// A reader's float parameter stored to a `float` (`FSTP dword`).
pub(crate) fn value_f32(reader: &mut IniReader, index: u32) -> f32 {
    reader.value_float(index) as f32
}
