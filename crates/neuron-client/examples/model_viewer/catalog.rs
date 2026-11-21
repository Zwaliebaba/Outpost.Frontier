//! What the viewer looks up in the data's tables: the ship and solar
//! archetypes (`SHIPS\shiparch.ini`, `SOLAR\solararch.ini`: a nickname's
//! `DA_archetype` and `material_library` files) and an effect's texture
//! libraries (the `[VisEffect]` tables), read with the engine's INI
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

/// The `[VisEffect]` tables among the `effects` files of the shipped
/// `OutpostFrontier.ini`, in its order.
pub const EFFECT_TABLES: [&[u8]; 8] = [
    b"FX\\ENGINES\\engines_ale.ini",
    b"FX\\EQUIPMENT\\equipment_ale.ini",
    b"FX\\EXPLOSIONS\\explosions_ale.ini",
    b"FX\\HULL_HITS\\hull_hits_ale.ini",
    b"FX\\MISC\\misc_ale.ini",
    b"FX\\SHIELDS\\shields_ale.ini",
    b"FX\\SPACE\\space_ale.ini",
    b"FX\\WEAPONS\\weapons_ale.ini",
];

/// The texture libraries (`textures`) of the `[VisEffect]` that plays
/// `effect` of the file `particlefx`, as the game loads them with the
/// effect; without such an entry, those of every entry of that file.
/// Names compare without case and with either slash.
#[must_use]
pub fn effect_textures(data: &Host, particlefx: &[u8], effect: &[u8]) -> Vec<Vec<u8>> {
    let same_path = |a: &[u8], b: &[u8]| {
        a.len() == b.len()
            && a.iter().zip(b).all(|(x, y)| {
                let fold = |c: u8| {
                    if c == b'/' {
                        b'\\'
                    } else {
                        c.to_ascii_lowercase()
                    }
                };
                fold(*x) == fold(*y)
            })
    };
    let mut named = Vec::new();
    let mut any = Vec::new();
    for table in EFFECT_TABLES {
        let Some(mut reader) = reader(data, table) else {
            continue;
        };
        while reader.read_header() {
            if !reader.is_header(b"VisEffect") {
                continue;
            }
            let (mut nickname, mut file, mut textures) = (Vec::new(), Vec::new(), Vec::new());
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    nickname = reader.value_string();
                } else if reader.is_value(b"particlefx") {
                    file = reader.value_string();
                } else if reader.is_value(b"textures") {
                    textures.push(reader.value_string());
                }
            }
            if !same_path(&file, particlefx) {
                continue;
            }
            let list = if eq_ignore_case(&nickname, effect) {
                &mut named
            } else {
                &mut any
            };
            for t in textures {
                if !list.iter().any(|known: &Vec<u8>| same_path(known, &t)) {
                    list.push(t);
                }
            }
        }
    }
    if named.is_empty() { any } else { named }
}
