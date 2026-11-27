//! The key map (`menu_keymap.cpp`, `menu_keymap_config.h`; work package
//! P5.2): the commands of `interface/KeyMap.ini` with their bindings, the
//! accelerator groups of its `[KeyMap]` sections, the player's
//! `UserKeyMap.ini` over them, and the active groups, newest first.
//!
//! This first increment holds the loaders, the lookups and the group
//! registration; the routing of key messages to commands follows with the
//! screens that use it. The files are read by the caller, so the loaders take
//! their text (`None` when the file does not open).

pub mod names;
pub mod nicknames;

use neuron_core::diag::{self, DumpFlags};
use neuron_core::hash::stricmp;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::atoi;
use neuron_core::num::trunc_to_i32;

pub use nicknames::NICKNAMES;

/// `kKeyCmdStateKeyDown`.
pub const STATE_KEY_DOWN: u32 = 1;
/// `kKeyCmdStateKeyUp`.
pub const STATE_KEY_UP: u32 = 2;
/// `kKeyCmdStateRepeat`.
pub const STATE_REPEAT: u32 = 4;
/// `kKeyCmdStateShiftIgnore`.
pub const STATE_SHIFT_IGNORE: u32 = 8;

/// `kKeyModShift`.
pub const MOD_SHIFT: u32 = 1;
/// `kKeyModControl`.
pub const MOD_CONTROL: u32 = 4;
/// `kKeyModAlt`.
pub const MOD_ALT: u32 = 0x10;

/// The base key map's path under the data root (`GetKeyMapIniRelPath`).
pub const KEY_MAP_PATH: &str = "interface/KeyMap.ini";

/// The player's key map in the user folder (`EnsureUserKeyMapPath`).
pub const USER_KEY_MAP_FILE: &str = "UserKeyMap.ini";

/// One binding (`FKeyBinding`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyBinding {
    /// The virtual key.
    pub virtual_key: i32,
    /// The modifiers: one of [`MOD_SHIFT`], [`MOD_CONTROL`], [`MOD_ALT`] or 0.
    pub modifiers: u32,
}

/// One command (`FKeyCmd`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeyCmd {
    /// The index into [`NICKNAMES`].
    pub nickname_id: u32,
    /// `ids_name`.
    pub ids_name: u32,
    /// `ids_info`.
    pub ids_info: u32,
    /// The `EKeyCmdState` bits.
    pub state: u32,
    /// The bindings, in file order.
    pub bindings: Vec<KeyBinding>,
}

impl Default for KeyCmd {
    /// The template every new command is copied from: all zero but the state,
    /// which is key-down.
    fn default() -> Self {
        Self {
            nickname_id: 0,
            ids_name: 0,
            ids_info: 0,
            state: STATE_KEY_DOWN,
            bindings: Vec::new(),
        }
    }
}

/// One `[KeyMap]` group (`FKeyMapGroup`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KeyMapGroup {
    /// The nickname, as written.
    pub nickname: Vec<u8>,
    /// Its commands, by index into [`KeyMap::commands`].
    pub commands: Vec<usize>,
}

/// What [`KeyMap::register_group`] and [`KeyMap::unregister_group`] found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GroupResult {
    /// The group is active (or, for an unregister, no longer).
    Done,
    /// No group has the name: the C++ shows `MessageBoxA(nullptr, "Invalid
    /// accelerator", "Error:", MB_OK)`, which the caller does.
    Invalid,
}

/// The two files [`KeyMap::ensure_loaded`] reads.
pub trait KeyMapFiles {
    /// `interface/KeyMap.ini` under the data root and the path to report.
    fn base(&mut self) -> (Option<Vec<u8>>, String);
    /// `UserKeyMap.ini` in the user folder.
    fn user(&mut self) -> Option<Vec<u8>>;
}

/// The key map's state (`GKeyCmds`, `GKeyMapGroups`, `GActiveGroups`,
/// `G_bKeyMapLoaded`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KeyMap {
    /// Every command, in load order.
    pub commands: Vec<KeyCmd>,
    /// Every group, in load order.
    pub groups: Vec<KeyMapGroup>,
    /// The active groups by index into [`KeyMap::groups`], newest first.
    pub active: Vec<usize>,
    loaded: bool,
}

/// The id of a nickname: the first of [`NICKNAMES`] equal to it ignoring
/// case, 0 (`USER_NONE`) for none.
#[must_use]
pub fn nickname_id(name: &[u8]) -> u32 {
    NICKNAMES
        .iter()
        .position(|n| stricmp(name, n.as_bytes()).is_eq())
        .and_then(|i| u32::try_from(i).ok())
        .unwrap_or(0)
}

/// `KeyMapGetActionNickname`: the nickname of an id.
#[must_use]
pub fn nickname(id: u32) -> &'static str {
    usize::try_from(id)
        .ok()
        .and_then(|i| NICKNAMES.get(i))
        .copied()
        .unwrap_or(NICKNAMES[0])
}

/// A C string's bytes: up to the first NUL.
fn c_str(bytes: &[u8]) -> &[u8] {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    &bytes[..end]
}

/// `get_value_float(index)` through `ftol`: the float the reader returns,
/// truncated.
fn float_as_u32(reader: &mut IniReader, index: u32) -> u32 {
    // `float INI_Reader::get_value_float`: the parsed number stored to a float.
    #[allow(clippy::cast_possible_truncation)]
    let narrowed = reader.value_float(index) as f32;
    trunc_to_i32(f64::from(narrowed)).cast_unsigned()
}

/// `sscanf(token, "%X", &value)`: hexadecimal digits after an optional
/// `0x`; `None` when no digit is read.
fn scan_hex(token: &[u8]) -> Option<u32> {
    let mut digits = token;
    if let [b'0', b'x' | b'X', rest @ ..] = token
        && rest.first().is_some_and(u8::is_ascii_hexdigit)
    {
        digits = rest;
    }
    let count = digits.iter().take_while(|b| b.is_ascii_hexdigit()).count();
    if count == 0 {
        return None;
    }
    Some(digits[..count].iter().fold(0u32, |v, &b| {
        let d = char::from(b).to_digit(16).unwrap_or(0);
        v.wrapping_mul(16).wrapping_add(d)
    }))
}

/// `ParseKeyValue`: a `key` value into its binding. A quoted first field is
/// its first character; one starting with `0` is hexadecimal; anything else
/// goes through `atoi`, with a report outside -8 to 255. Then every further
/// field names a modifier (`shift`, `control`, `alt` by prefix, the last one
/// winning); any other word fails.
fn parse_key_value(reader: &IniReader) -> Option<KeyBinding> {
    let token = reader.value_string_at(0);
    let token = c_str(&token);
    let mut binding = KeyBinding::default();
    match token.first()? {
        b'"' => binding.virtual_key = i32::from(token.get(1).copied().unwrap_or(0)),
        b'0' => binding.virtual_key = scan_hex(token)?.cast_signed(),
        _ => {
            let parsed = atoi(token);
            if !(-8..=0xff).contains(&parsed) {
                diag::dump(
                    DumpFlags::ERROR,
                    &format!(
                        "Client\\Interface\\KeyMap.cpp(486) : *** ERROR: invalid KeyMap key value ({})",
                        String::from_utf8_lossy(token)
                    ),
                );
            }
            binding.virtual_key = parsed;
        }
    }
    let mut index = 1;
    while !reader.is_value_empty(index) {
        let field = reader.value_string_at(index);
        let field = c_str(&field);
        if field.is_empty() {
            break;
        }
        let starts = |word: &[u8]| {
            field.len() >= word.len() && field[..word.len()].eq_ignore_ascii_case(word)
        };
        binding.modifiers = if starts(b"shift") {
            MOD_SHIFT
        } else if starts(b"control") {
            MOD_CONTROL
        } else if starts(b"alt") {
            MOD_ALT
        } else {
            return None;
        };
        index += 1;
    }
    Some(binding)
}

/// `ParseKeyCmdSection`: the current `[KeyCmd]` into `cmd`. Only the id is
/// reset; an unknown nickname fails at once.
fn parse_key_cmd(reader: &mut IniReader, cmd: &mut KeyCmd) -> bool {
    cmd.nickname_id = 0;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            cmd.nickname_id = nickname_id(c_str(&reader.value_string()));
            if cmd.nickname_id == 0 {
                return false;
            }
        } else if reader.is_value(b"ids_name") {
            cmd.ids_name = float_as_u32(reader, 0);
        } else if reader.is_value(b"ids_info") {
            cmd.ids_info = float_as_u32(reader, 0);
        } else if reader.is_value(b"state") {
            let mut mask = 0;
            if !reader.is_value_empty(0) {
                let mut index = 0;
                loop {
                    let word = reader.value_string_at(index);
                    let word = c_str(&word);
                    for (name, bit) in [
                        (&b"keydown"[..], STATE_KEY_DOWN),
                        (b"keyup", STATE_KEY_UP),
                        (b"repeat", STATE_REPEAT),
                        (b"shift_ignore", STATE_SHIFT_IGNORE),
                    ] {
                        if stricmp(word, name).is_eq() {
                            mask |= bit;
                        }
                    }
                    index += 1;
                    if reader.is_value_empty(index) {
                        break;
                    }
                }
            }
            cmd.state = if mask == 0 { STATE_KEY_DOWN } else { mask };
        } else if reader.is_value(b"key")
            && let Some(binding) = parse_key_value(reader)
        {
            cmd.bindings.push(binding);
        }
    }
    cmd.nickname_id != 0
}

impl KeyMap {
    /// An empty key map, not loaded.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `FindKeyCmdByNicknameId`: the first command with this id.
    #[must_use]
    pub fn find_by_id(&self, id: u32) -> Option<usize> {
        self.commands.iter().position(|c| c.nickname_id == id)
    }

    /// `FindKeyCmdByNickname`.
    #[must_use]
    pub fn find_by_nickname(&self, name: &[u8]) -> Option<usize> {
        self.find_by_id(nickname_id(name))
    }

    /// `FindKeyMapGroup`: the first group with this nickname, ignoring case.
    #[must_use]
    pub fn find_group(&self, name: &[u8]) -> Option<usize> {
        self.groups
            .iter()
            .position(|g| stricmp(&g.nickname, name).is_eq())
    }

    /// `KeyMapFindCommandBinding`: binding `index` of the first command with
    /// this id that has that many.
    #[must_use]
    pub fn binding(&self, id: u32, index: usize) -> Option<KeyBinding> {
        self.commands
            .iter()
            .filter(|c| c.nickname_id == id)
            .find_map(|c| c.bindings.get(index).copied())
    }

    /// `LoadKeyMapIni(path, fatal)`: every `[KeyCmd]` and `[KeyMap]` of the
    /// file appended to the lists, nothing cleared; the answer is the number
    /// of `[KeyMap]` sections, 0 when the file did not open. A first header
    /// other than `[KeyMap=1.1]` is reported, and that section is skipped.
    pub fn load_base(&mut self, text: Option<&[u8]>, path: &str) -> i32 {
        let mut reader = IniReader::new();
        let Some(text) = text else {
            return 0;
        };
        if !reader.open(path.as_bytes(), text.to_vec()) {
            return 0;
        }
        if !reader.read_header() || !reader.is_header(b"KeyMap=1.1") {
            diag::dump(
                DumpFlags::ERROR,
                &format!(
                    "Client\\Interface\\KeyMap.cpp(746) : *** ERROR: KeyMap file ({path}) is not version 1.1"
                ),
            );
        }
        let mut groups = 0;
        while reader.read_header() {
            if reader.is_header(b"KeyCmd") {
                let mut cmd = KeyCmd::default();
                if parse_key_cmd(&mut reader, &mut cmd) {
                    self.commands.push(cmd);
                }
            } else if reader.is_header(b"KeyMap") {
                groups += 1;
                let mut group = KeyMapGroup::default();
                while reader.read_value() {
                    if reader.is_value(b"nickname") {
                        group.nickname = c_str(&reader.value_string()).to_vec();
                    } else if reader.is_value(b"key") {
                        let name = reader.value_string_at(0);
                        if let Some(cmd) = self.find_by_nickname(c_str(&name)) {
                            group.commands.push(cmd);
                        }
                    }
                }
                self.groups.push(group);
            }
        }
        groups
    }

    /// `LoadUserKeyMapIni(path)`: with a leading `[KeyMap=1.1]`, every
    /// binding cleared, then each `[KeyCmd]`'s keys appended to the first
    /// command with its nickname; the answer is the number of `[KeyCmd]`
    /// sections, 0 when the file did not open or has no such header.
    pub fn load_user(&mut self, text: Option<&[u8]>) -> i32 {
        let mut reader = IniReader::new();
        let Some(text) = text else {
            return 0;
        };
        if !reader.open(USER_KEY_MAP_FILE.as_bytes(), text.to_vec()) {
            return 0;
        }
        if !reader.read_header() || !reader.is_header(b"KeyMap=1.1") {
            return 0;
        }
        for cmd in &mut self.commands {
            cmd.bindings.clear();
        }
        let mut sections = 0;
        while reader.read_header() {
            if !reader.is_header(b"KeyCmd") {
                continue;
            }
            let mut selected = None;
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    selected = self.find_by_id(nickname_id(c_str(&reader.value_string())));
                } else if reader.is_value(b"key")
                    && let Some(index) = selected
                    && let Some(binding) = parse_key_value(&reader)
                {
                    self.commands[index].bindings.push(binding);
                }
            }
            sections += 1;
        }
        sections
    }

    /// `EnsureKeyMapLoaded`: the base map once it loads, then the player's;
    /// `on_loaded` sees the map after each (the oracle's
    /// `OracleOnKeyMapLoaded("base"` and `"user")`). A base map that loads no
    /// group leaves the map unloaded, to be tried again.
    pub fn ensure_loaded(
        &mut self,
        files: &mut dyn KeyMapFiles,
        on_loaded: &mut dyn FnMut(&Self, &str, i32),
    ) {
        if self.loaded {
            return;
        }
        let (text, path) = files.base();
        let loaded = self.load_base(text.as_deref(), &path);
        on_loaded(self, "base", loaded);
        if loaded == 0 {
            return;
        }
        self.loaded = true;
        let user = files.user();
        let result = self.load_user(user.as_deref());
        on_loaded(self, "user", result);
    }

    /// `RegisterAcceleratorGroup(name)`: after [`Self::ensure_loaded`], the
    /// group made active at the front unless it already is.
    pub fn register_group(
        &mut self,
        name: &[u8],
        files: &mut dyn KeyMapFiles,
        on_loaded: &mut dyn FnMut(&Self, &str, i32),
    ) -> GroupResult {
        self.ensure_loaded(files, on_loaded);
        let Some(group) = self.find_group(name) else {
            return GroupResult::Invalid;
        };
        if !self.active.contains(&group) {
            self.active.insert(0, group);
        }
        GroupResult::Done
    }

    /// `UnregisterAcceleratorGroup(name)`: nothing while no group is active;
    /// otherwise the first entry of the group leaves the active list.
    pub fn unregister_group(
        &mut self,
        name: &[u8],
        files: &mut dyn KeyMapFiles,
        on_loaded: &mut dyn FnMut(&Self, &str, i32),
    ) -> GroupResult {
        if self.active.is_empty() {
            return GroupResult::Done;
        }
        self.ensure_loaded(files, on_loaded);
        let Some(group) = self.find_group(name) else {
            return GroupResult::Invalid;
        };
        if let Some(at) = self.active.iter().position(|&g| g == group) {
            self.active.remove(at);
        }
        GroupResult::Done
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const BASE: &[u8] = b"[KeyMap=1.1]\n\
[KeyCmd]\nnickname = USER_NN\nids_name = 1436\nids_info = 3200\nstate = keydown\nkey = \"N\"\n\
[KeyCmd]\nnickname = USER_NOT_A_COMMAND\nkey = 65\n\
[KeyCmd]\nnickname = user_cancel\nstate = keydown, keyup, nonsense\nkey = 27\nkey = 0x41, SHIFT\nkey = 66, ctrl\n\
[KeyCmd]\nnickname = USER_SCREEN_SHOT\nstate =\nkey = 300\n\
[KeyMap]\nnickname = IDR_ALWAYS_PRESENT\nkey = USER_CANCEL\nkey = USER_MISSING\nkey = USER_NN\n";

    struct Files(Option<Vec<u8>>, Option<Vec<u8>>);

    impl KeyMapFiles for Files {
        fn base(&mut self) -> (Option<Vec<u8>>, String) {
            (self.0.clone(), "data/interface/KeyMap.ini".into())
        }
        fn user(&mut self) -> Option<Vec<u8>> {
            self.1.clone()
        }
    }

    #[test]
    fn the_base_map_keeps_known_commands_and_their_bindings() {
        let mut map = KeyMap::new();
        assert_eq!(map.load_base(Some(BASE), "KeyMap.ini"), 1);
        assert_eq!(map.commands.len(), 3);
        let nn = &map.commands[0];
        assert_eq!(nickname(nn.nickname_id), "USER_NN");
        assert_eq!((nn.ids_name, nn.ids_info, nn.state), (1436, 3200, 1));
        assert_eq!(
            nn.bindings,
            [KeyBinding {
                virtual_key: 0x4e,
                modifiers: 0
            }]
        );
        let cancel = &map.commands[1];
        assert_eq!(nickname(cancel.nickname_id), "USER_CANCEL");
        assert_eq!(cancel.state, STATE_KEY_DOWN | STATE_KEY_UP);
        // "ctrl" is no modifier, so that binding is dropped.
        assert_eq!(
            cancel.bindings,
            [
                KeyBinding {
                    virtual_key: 27,
                    modifiers: 0
                },
                KeyBinding {
                    virtual_key: 0x41,
                    modifiers: MOD_SHIFT
                },
            ]
        );
        // An empty state is key-down; 300 is out of range but kept.
        assert_eq!(map.commands[2].state, STATE_KEY_DOWN);
        assert_eq!(map.commands[2].bindings[0].virtual_key, 300);
        assert_eq!(map.groups[0].nickname, b"IDR_ALWAYS_PRESENT");
        assert_eq!(map.groups[0].commands, [1, 0]);
    }

    #[test]
    fn the_user_map_replaces_every_binding() {
        let mut map = KeyMap::new();
        map.load_base(Some(BASE), "KeyMap.ini");
        let user = b"[KeyMap=1.1]\n[KeyCmd]\nnickname = USER_NN\nkey = 77, ALT\n[Other]\n[KeyCmd]\nkey = 1\n";
        assert_eq!(map.load_user(Some(user)), 2);
        assert_eq!(
            map.commands[0].bindings,
            [KeyBinding {
                virtual_key: 77,
                modifiers: MOD_ALT
            }]
        );
        assert!(map.commands[1].bindings.is_empty());
        assert_eq!(map.load_user(Some(b"[KeyCmd]\n")), 0);
        assert_eq!(map.load_user(None), 0);
    }

    #[test]
    fn loading_runs_once_and_groups_register_at_the_front() {
        let mut map = KeyMap::new();
        let mut files = Files(Some(BASE.to_vec()), None);
        let mut seen = Vec::new();
        let mut note = |m: &KeyMap, phase: &str, result: i32| {
            seen.push((phase.to_string(), result, m.active.len()));
        };
        assert_eq!(
            map.register_group(b"idr_always_present", &mut files, &mut note),
            GroupResult::Done
        );
        assert_eq!(
            map.register_group(b"IDR_NOPE", &mut files, &mut note),
            GroupResult::Invalid
        );
        assert_eq!(map.active, [0]);
        assert_eq!(
            map.unregister_group(b"IDR_ALWAYS_PRESENT", &mut files, &mut note),
            GroupResult::Done
        );
        assert!(map.active.is_empty());
        assert_eq!(
            seen,
            [("base".to_string(), 1, 0), ("user".to_string(), 0, 0)]
        );
    }

    #[test]
    fn hex_reads_as_sscanf_x() {
        assert_eq!(scan_hex(b"0x41"), Some(0x41));
        assert_eq!(scan_hex(b"0"), Some(0));
        assert_eq!(scan_hex(b"0x"), Some(0));
        assert_eq!(scan_hex(b"0FF"), Some(0xff));
    }
}
