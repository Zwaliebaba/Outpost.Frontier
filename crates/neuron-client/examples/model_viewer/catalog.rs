//! What the viewer looks up in the data's tables: the ship and solar
//! archetypes (`SHIPS\shiparch.ini`, `SOLAR\solararch.ini`: a nickname's
//! `DA_archetype` and `material_library` files), read with the engine's INI
//! reader. Sounds and voice lines come from the engine's sound tables
//! (`Engine::load_sound_tables`).

use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

/// A model archetype of a table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModelEntry {
    /// The `DA_archetype` file.
    pub archetype: Vec<u8>,
    /// The `material_library` files, in order.
    pub libraries: Vec<Vec<u8>>,
}

fn reader(data: &Host, path: &[u8]) -> Option<IniReader> {
    let file = data.read_file(path).ok()?;
    let mut reader = IniReader::new();
    reader.open(path, file.data).then_some(reader)
}

/// The archetype of `nickname` in the sections `header` of `table`.
#[must_use]
pub fn model(data: &Host, table: &[u8], header: &[u8], nickname: &[u8]) -> Option<ModelEntry> {
    let mut reader = reader(data, table)?;
    while reader.read_header() {
        if !reader.is_header(header) {
            continue;
        }
        let mut found = false;
        let mut entry = ModelEntry {
            archetype: Vec::new(),
            libraries: Vec::new(),
        };
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                found = eq_ignore_case(&reader.value_string(), nickname);
            } else if reader.is_value(b"DA_archetype") {
                entry.archetype = reader.value_string();
            } else if reader.is_value(b"material_library") {
                entry.libraries.push(reader.value_string());
            }
        }
        if found && !entry.archetype.is_empty() {
            return Some(entry);
        }
    }
    None
}

/// A ship archetype by nickname.
#[must_use]
pub fn ship(data: &Host, nickname: &[u8]) -> Option<ModelEntry> {
    model(data, b"SHIPS\\shiparch.ini", b"Ship", nickname)
}

/// A solar archetype (a station, a planet, a dock) by nickname.
#[must_use]
pub fn solar(data: &Host, nickname: &[u8]) -> Option<ModelEntry> {
    model(data, b"SOLAR\\solararch.ini", b"Solar", nickname)
}
