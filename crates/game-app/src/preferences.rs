//! The preferences screen's data lists (`NN_Preferences`, `nn_preferences.cpp`;
//! work package P5.2): the key list of `keylist.ini` with each command's
//! bindings from the key map, and the option list of `optlist.ini`. The
//! screen's controls come with the toolkit's screens; these are the loaders
//! its `Create` and the start-up's `SeedGraphicsDefaults` run.

use neuron_core::diag::{self, DumpFlags};
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::atoi;

use crate::keymap::KeyMap;

/// `kKeyListPath`, relative to the client's folder.
pub const KEY_LIST_PATH: &str = "data/interface/keylist.ini";

/// `kOptListPath`, relative to the client's folder.
pub const OPT_LIST_PATH: &str = "data/interface/optlist.ini";

/// One binding of a key row (`FKeyRowKey`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyRowKey {
    /// The command's id, as the lookup wrote it.
    pub nickname_id: u32,
    /// The virtual key.
    pub virtual_key: i32,
    /// The modifiers.
    pub modifiers: u32,
}

/// One key row (`FKeyRow`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyRow {
    /// The primary binding.
    pub primary: KeyRowKey,
    /// The alternative binding.
    pub alt: KeyRowKey,
    /// The caption's text id, -1 for none.
    pub name_ids: i32,
    /// The rollover's text id, -1 for none.
    pub info_ids: i32,
    /// The command's id, 0 for none.
    pub command_id: u32,
    /// 0 no command, 1 a primary binding only, 2 an alternative too, 3 no
    /// primary binding.
    pub binding_kind: i32,
    /// The row's bindings may be changed.
    pub editable: bool,
}

/// One option row (`FPrefOption`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PrefOption {
    /// The caption's text id, also the option's key.
    pub id: i32,
    /// The rollover's text id.
    pub info_ids: i32,
    /// The default, -1 for none.
    pub default: i32,
    /// The minimum.
    pub min: i32,
    /// The maximum.
    pub max: i32,
    /// A slider, else a toggle.
    pub slider: bool,
    /// `Byte15`.
    pub byte15: bool,
    /// A caption without a value.
    pub header: bool,
}

impl Default for PrefOption {
    /// `BlankOption`.
    fn default() -> Self {
        Self {
            id: -1,
            info_ids: -1,
            default: -1,
            min: -1,
            max: -1,
            slider: false,
            byte15: false,
            header: false,
        }
    }
}

/// The screen's lists (`NnPreferences`'s `GroupNameIds`, `KeyGroups`,
/// `OptionsA` to `OptionsC` and `bSeedMode`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Preferences {
    /// The three key groups' caption ids.
    pub group_name_ids: [i32; 3],
    /// The three key groups' rows.
    pub key_groups: [Vec<KeyRow>; 3],
    /// `[Interface]`'s options.
    pub interface: Vec<PrefOption>,
    /// `[Audio]`'s options.
    pub audio: Vec<PrefOption>,
    /// `[Video]`'s options.
    pub video: Vec<PrefOption>,
    /// `bSeedMode`: the start-up's bare screen, whose option list seeds the
    /// performance options.
    pub seed_mode: bool,
}

/// A C string's bytes: up to the first NUL.
fn c_str(bytes: &[u8]) -> &[u8] {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    &bytes[..end]
}

impl Preferences {
    /// `NnPreferences::LoadKeyList(path)`: the three groups emptied, then
    /// each `[Group]` (its number and caption; group 1 opens with a caption
    /// row) and each `[Key]` (a command by nickname, or a separator for
    /// `-1`) read in order. A file that does not open is reported and leaves
    /// the groups empty.
    pub fn load_key_list(&mut self, text: Option<&[u8]>, path: &str, keymap: &KeyMap) {
        for group in &mut self.key_groups {
            group.clear();
        }
        let mut reader = IniReader::new();
        let opened = text.is_some_and(|t| reader.open(path.as_bytes(), t.to_vec()));
        if !opened {
            diag::dump(
                DumpFlags::ERROR,
                &format!("Client\\nn\\NN_Preferences.cpp(2320) : *** ERROR: Could not open {path}"),
            );
            return;
        }
        let mut group: i32 = 0;
        while reader.read_header() {
            if reader.is_header(b"Group") {
                group = -1;
                while reader.read_value() {
                    if reader.is_value(b"group_num") {
                        group = reader.value_int(0);
                    } else if reader.is_value(b"name") {
                        // The C++ indexes the three-entry array without a test (D1100).
                        if let Some(slot) = usize::try_from(group)
                            .ok()
                            .and_then(|g| self.group_name_ids.get_mut(g))
                        {
                            *slot = reader.value_int(0);
                        }
                    }
                }
                if group == 1 {
                    self.key_groups[1].push(KeyRow {
                        primary: KeyRowKey {
                            nickname_id: 0,
                            virtual_key: -1,
                            modifiers: 0,
                        },
                        name_ids: 0xc2e,
                        info_ids: 0xcf7,
                        command_id: 0,
                        binding_kind: group,
                        editable: false,
                        ..KeyRow::default()
                    });
                }
            }
            if reader.is_header(b"Key") {
                let mut command: i32 = -1;
                while reader.read_value() {
                    if reader.is_value(b"id") {
                        let nickname = reader.value_string();
                        let nickname = c_str(&nickname);
                        if atoi(nickname) == -1 {
                            command = -1;
                        } else if let Some(cmd) = keymap.find_by_nickname(nickname) {
                            command = keymap.commands[cmd].nickname_id.cast_signed();
                        }
                    }
                }
                self.add_key_row(command, group, keymap);
            }
        }
    }

    /// `NnPreferences::AddKeyRow(command, category)`: a row for the command's
    /// bindings and texts in a group 0 to 2; category 3 builds it and keeps
    /// nothing.
    fn add_key_row(&mut self, command: i32, category: i32, keymap: &KeyMap) {
        if !(0..=3).contains(&category) {
            return;
        }
        let mut row = KeyRow {
            editable: true,
            ..KeyRow::default()
        };
        let mut name_ids = -1;
        let mut info_ids = -1;
        if let Ok(id) = u32::try_from(command) {
            let primary = keymap.binding(id, 0);
            let found = keymap.find_by_id(id).is_some();
            // `KeyMapFindCommandBinding` writes the id as soon as a command
            // has it, whether or not the binding exists.
            let lookup_id = if found { id } else { 0 };
            if let Some(binding) = primary {
                row.primary = KeyRowKey {
                    nickname_id: lookup_id,
                    virtual_key: binding.virtual_key,
                    modifiers: binding.modifiers,
                };
                let alt = keymap.binding(id, 1);
                row.alt = KeyRowKey {
                    nickname_id: lookup_id,
                    virtual_key: alt.map_or(0, |b| b.virtual_key),
                    modifiers: alt.map_or(0, |b| b.modifiers),
                };
                row.binding_kind = if alt.is_some() { 2 } else { 1 };
            } else {
                row.primary.nickname_id = lookup_id;
                row.binding_kind = 3;
            }
            if let Some(cmd) = keymap.find_by_id(id).map(|i| &keymap.commands[i]) {
                if cmd.ids_name != 0 {
                    name_ids = cmd.ids_name.cast_signed();
                    if id == 0x14 {
                        row.editable = false;
                    }
                }
                if cmd.ids_info != 0 {
                    info_ids = cmd.ids_info.cast_signed();
                }
            }
            row.command_id = id;
        } else {
            row.binding_kind = 0;
            row.command_id = 0;
        }
        row.name_ids = name_ids;
        row.info_ids = info_ids;
        if let Some(group) = usize::try_from(category)
            .ok()
            .and_then(|c| self.key_groups.get_mut(c))
        {
            group.push(row);
        }
    }

    /// `NnPreferences::LoadOptList(path)`: the `[Interface]`, `[Audio]` and
    /// `[Video]` options appended to their lists (never cleared), each
    /// section padded to 15 rows when it has 14 or fewer. In seed mode each
    /// option's default seeds the performance option of its id (`seed`, with
    /// `float(default * 0.01)`). False when the file does not open, which is
    /// reported.
    pub fn load_opt_list(
        &mut self,
        text: Option<&[u8]>,
        path: &str,
        seed: &mut dyn FnMut(i32, f32),
    ) -> bool {
        let mut reader = IniReader::new();
        let opened = text.is_some_and(|t| reader.open(path.as_bytes(), t.to_vec()));
        if !opened {
            diag::dump(
                DumpFlags::ERROR,
                &format!("Client\\nn\\NN_Preferences.cpp(2549) : *** ERROR: Could not open {path}"),
            );
            return false;
        }
        let seed_mode = self.seed_mode;
        while reader.read_header() {
            if reader.is_header(b"Interface") {
                read_opt_section(&mut reader, &mut self.interface, seed_mode, seed);
            }
            if reader.is_header(b"Audio") {
                read_opt_section(&mut reader, &mut self.audio, seed_mode, seed);
            }
            if reader.is_header(b"Video") {
                read_opt_section(&mut reader, &mut self.video, seed_mode, seed);
            }
        }
        true
    }
}

/// `ReadOptSection`: one section's `option` lines, then blank rows up to
/// 15 when it had 14 or fewer.
fn read_opt_section(
    reader: &mut IniReader,
    options: &mut Vec<PrefOption>,
    seed_mode: bool,
    seed: &mut dyn FnMut(i32, f32),
) {
    let mut count = 0;
    while reader.read_value() {
        if !reader.is_value(b"option") {
            continue;
        }
        let mut option = PrefOption::default();
        parse_opt_line(&mut option, reader, seed_mode, seed);
        count += 1;
        options.push(option);
    }
    if count > 14 {
        return;
    }
    for _ in count..15 {
        options.push(PrefOption::default());
    }
}

/// `ParseOptLine`: `option = id[, info, default, slider, byte15[, min,
/// max]]`; a lone id is a caption.
fn parse_opt_line(
    option: &mut PrefOption,
    reader: &mut IniReader,
    seed_mode: bool,
    seed: &mut dyn FnMut(i32, f32),
) {
    let count = reader.num_parameters();
    option.id = reader.value_int(0);
    if count > 1 {
        option.info_ids = reader.value_int(1);
        option.default = reader.value_int(2);
        option.slider = reader.value_bool(3);
        option.byte15 = reader.value_bool(4);
        if count > 5 {
            option.min = reader.value_int(5);
            option.max = reader.value_int(6);
        }
    } else {
        option.header = true;
    }
    if seed_mode {
        // `static_cast<float>(Default * 0.01)`.
        #[allow(clippy::cast_possible_truncation)]
        let value = (f64::from(option.default) * 0.01) as f32;
        seed(option.id, value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MAP: &[u8] = b"[KeyMap=1.1]\n\
[KeyCmd]\nnickname = USER_CRUISE\nids_name = 10\nids_info = 11\nkey = 87\nkey = 38\n\
[KeyCmd]\nnickname = USER_AFTERBURN\nids_name = 12\nkey = 9\n\
[KeyCmd]\nnickname = USER_NN\n";

    #[test]
    fn key_rows_take_the_commands_bindings_and_texts() {
        let mut map = KeyMap::new();
        map.load_base(Some(MAP), "KeyMap.ini");
        let list =
            b"[Group]\ngroup_num = 0\nname = 1504\n[key]\nid = USER_CRUISE\n[key]\nid = -1\n\
[key]\nid = USER_AFTERBURN\n[key]\nid = USER_NN\n[key]\nid = USER_UNKNOWN\n\
[Group]\nname = 7\ngroup_num = 1\n[key]\nid = USER_CRUISE\n";
        let mut p = Preferences::default();
        p.load_key_list(Some(list), KEY_LIST_PATH, &map);
        // The name before group_num writes no group (D1100).
        assert_eq!(p.group_name_ids, [1504, 0, 0]);
        let g0 = &p.key_groups[0];
        assert_eq!(g0.len(), 5);
        let cruise = map
            .find_by_nickname(b"USER_CRUISE")
            .map(|i| map.commands[i].nickname_id);
        assert_eq!(g0[0].binding_kind, 2);
        assert_eq!((g0[0].primary.virtual_key, g0[0].alt.virtual_key), (87, 38));
        assert_eq!(Some(g0[0].command_id), cruise);
        assert_eq!((g0[0].name_ids, g0[0].info_ids), (10, 11));
        assert_eq!(
            (g0[1].binding_kind, g0[1].command_id, g0[1].name_ids),
            (0, 0, -1)
        );
        assert_eq!((g0[2].binding_kind, g0[2].info_ids), (1, -1));
        assert_eq!(g0[3].binding_kind, 3);
        // An unknown nickname keeps the command at -1: a separator.
        assert_eq!(g0[4].binding_kind, 0);
        // Group 1 opens with its caption row.
        let g1 = &p.key_groups[1];
        assert_eq!(
            (g1[0].name_ids, g1[0].info_ids, g1[0].binding_kind),
            (0xc2e, 0xcf7, 1)
        );
        assert!(!g1[0].editable && g1[1].editable);
    }

    #[test]
    fn option_sections_pad_to_fifteen_and_seed_their_defaults() {
        let list = b"[interface]\noption = 1894\noption = 1334, 2443, -1, true, false, -1, -1\n\
option = 1332, 2444, 30, true, false\n[Audio]\n[Video]\n";
        let mut p = Preferences {
            seed_mode: true,
            ..Preferences::default()
        };
        let mut seeded = Vec::new();
        assert!(p.load_opt_list(Some(list), OPT_LIST_PATH, &mut |id, v| seeded.push((id, v))));
        assert_eq!(
            (p.interface.len(), p.audio.len(), p.video.len()),
            (15, 15, 15)
        );
        assert!(p.interface[0].header);
        let o = p.interface[2];
        assert_eq!(
            (o.id, o.info_ids, o.default, o.min, o.max),
            (1332, 2444, 30, -1, -1)
        );
        assert!(o.slider && !o.byte15);
        assert_eq!(seeded[2], (1332, 0.3));
        assert!(!p.load_opt_list(None, OPT_LIST_PATH, &mut |_, _| {}));
    }
}
