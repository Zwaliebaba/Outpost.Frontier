//! What the viewer looks up in the data's tables: the ship and solar
//! archetypes (`SHIPS\shiparch.ini`, `SOLAR\solararch.ini`: a nickname's
//! `DA_archetype` and `material_library` files) and the sound tables
//! (`AUDIO\*.ini`: a nickname's `file`), read with the engine's INI reader.

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

/// The sound tables the viewer searches, in order.
pub const SOUND_TABLES: [&[u8]; 8] = [
    b"AUDIO\\interface_sounds.ini",
    b"AUDIO\\sounds.ini",
    b"AUDIO\\gf_sounds.ini",
    b"AUDIO\\engine_sounds.ini",
    b"AUDIO\\ambience_sounds.ini",
    b"AUDIO\\story_sounds.ini",
    b"AUDIO\\music.ini",
    b"AUDIO\\voices_base_male.ini",
];

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

/// The file of the sound `nickname` in the sound tables.
#[must_use]
pub fn sound(data: &Host, nickname: &[u8]) -> Option<Vec<u8>> {
    for table in SOUND_TABLES {
        let Some(mut reader) = reader(data, table) else {
            continue;
        };
        while reader.read_header() {
            if !reader.is_header(b"Sound") {
                continue;
            }
            let (mut found, mut file) = (false, None);
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    found = eq_ignore_case(&reader.value_string(), nickname);
                } else if reader.is_value(b"file") {
                    file = Some(reader.value_string());
                }
            }
            if found && file.is_some() {
                return file;
            }
        }
    }
    None
}

/// The voice lines (the RIFF leaves) of a UTF voice archive: each leaf's
/// path in the archive and its bytes.
#[must_use]
pub fn voice_lines(bytes: Vec<u8>) -> Vec<(String, Vec<u8>)> {
    use neuron_core::utf::{NodeRef, UtfArchive};
    fn walk(archive: &UtfArchive, node: NodeRef, path: &str, out: &mut Vec<(String, Vec<u8>)>) {
        for child in archive.children(node).flatten() {
            let name = archive
                .name(child)
                .map(|n| String::from_utf8_lossy(n).into_owned())
                .unwrap_or_default();
            let path = if path.is_empty() {
                name
            } else {
                format!("{path}\\{name}")
            };
            if archive.is_dir(child).unwrap_or(false) {
                walk(archive, child, &path, out);
            } else if let Ok(data) = archive.data(child)
                && data.starts_with(b"RIFF")
            {
                out.push((path, data.to_vec()));
            }
        }
    }
    let Ok(archive) = UtfArchive::parse(bytes) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    walk(&archive, archive.root(), "", &mut out);
    out
}
