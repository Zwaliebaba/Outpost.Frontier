//! The client's voice profiles (`Client\cmn\Voice.cpp`, ported in
//! `CppSource/OutpostFrontier/Client/voice_profiles.cpp`): the `[Voice]`
//! profiles of `OutpostFrontier.ini`'s `voices =` files, each with its lines
//! (`VoiceMsg`, a sound archetype of its own) and the archive its wavs live
//! in, `audio\<nickname>.utf` relative to the data (decision O31).
//!
//! A line of a profile with an archive is the archive's leaf named
//! `0x%X` of the line's id; without one it is the line's `file =`.

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::arch::{WavFactory, WavRequest};

/// One line of a profile (`VoiceMsg`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VoiceMsg {
    /// `CreateID(msg)`.
    pub id: u32,
    /// The mixer archetype, -1 until made.
    pub sm_arch_id: i32,
    /// `priority =`.
    pub priority: i8,
    /// `attenuation =`.
    pub attenuation: i8,
    /// The use count.
    pub ref_count: i16,
}

impl VoiceMsg {
    fn new() -> VoiceMsg {
        VoiceMsg {
            sm_arch_id: -1,
            ..VoiceMsg::default()
        }
    }
}

/// One `[Voice]` profile (`VoiceProfile`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VoiceProfile {
    /// `CreateID(nickname)`.
    pub id: u32,
    /// The nickname.
    pub name: Vec<u8>,
    /// The lines by id.
    pub messages: BTreeMap<u32, VoiceMsg>,
    /// `script =`, in order.
    pub scripts: Vec<Vec<u8>>,
    /// `audio\<nickname>.utf` when it exists.
    pub utf_path: Option<Vec<u8>>,
    /// `file =` per line id (ignored once the profile has an archive).
    pub sound_files: BTreeMap<u32, Vec<u8>>,
}

impl VoiceProfile {
    /// Slot 1 (`GetSoundFile`): `0x%X` of a line the profile has when it
    /// has an archive, else the line's `file =`; empty for neither.
    #[must_use]
    pub fn sound_file(&self, msg: u32) -> Vec<u8> {
        if self.utf_path.is_some() {
            return if self.messages.contains_key(&msg) {
                format!("0x{msg:X}").into_bytes()
            } else {
                Vec::new()
            };
        }
        self.sound_files.get(&msg).cloned().unwrap_or_default()
    }

    fn set_sound_file(&mut self, msg: u32, file: Vec<u8>) {
        if self.utf_path.is_none() {
            self.sound_files.insert(msg, file);
        }
    }
}

/// The voice profiles (`GVoiceProfiles`).
#[derive(Clone, Debug, Default)]
pub struct VoiceTable {
    profiles: BTreeMap<u32, VoiceProfile>,
}

impl VoiceTable {
    /// No profiles.
    #[must_use]
    pub fn new() -> VoiceTable {
        VoiceTable::default()
    }

    /// The profiles in id order.
    pub fn iter(&self) -> impl Iterator<Item = &VoiceProfile> {
        self.profiles.values()
    }

    /// `GetClientVoiceCount`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.profiles.len()
    }

    /// No profiles.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.profiles.is_empty()
    }

    /// `LookupClientVoice`.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&VoiceProfile> {
        self.profiles.get(&id)
    }

    /// A line of a profile.
    #[must_use]
    pub fn message(&self, profile: u32, msg: u32) -> Option<&VoiceMsg> {
        self.profiles.get(&profile)?.messages.get(&msg)
    }

    /// `ClearClientVoices`.
    pub fn clear(&mut self) {
        self.profiles.clear();
    }

    /// `LoadClientVoicesIni`: the `[Voice]` profiles of an INI file and the
    /// `[Sound]` (or `[VoiceSound]`) lines after each; `extend =` sends the
    /// lines to an earlier profile. `archive_exists` answers whether
    /// `audio\<nickname>.utf` exists in the data (`ResolveScriptFile`'s
    /// `_access`). The number of profiles added.
    pub fn load_ini(&mut self, bytes: &[u8], archive_exists: &dyn Fn(&[u8]) -> bool) -> usize {
        let before = self.profiles.len();
        let mut reader = IniReader::new();
        if !reader.open_memory(bytes) {
            return 0;
        }
        let mut current: Option<u32> = None;
        while reader.read_header() {
            if reader.is_header(b"Voice") {
                let mut local = VoiceProfile::default();
                let mut extend: Vec<u8> = Vec::new();
                while reader.read_value() {
                    if reader.is_value(b"nickname") {
                        let nick = reader.value_string();
                        local.id = create_id(&nick);
                        local.name = nick;
                    } else if reader.is_value(b"extend") {
                        // CopyBoundedString into 0x80 bytes.
                        let mut value = reader.value_string();
                        value.truncate(0x7F);
                        extend = value;
                    } else if reader.is_value(b"script") {
                        local.scripts.push(reader.value_string());
                    }
                }
                if extend.is_empty() {
                    let id = local.id;
                    if let std::collections::btree_map::Entry::Vacant(slot) =
                        self.profiles.entry(id)
                    {
                        let profile = slot.insert(local);
                        let mut path = b"audio\\".to_vec();
                        path.extend_from_slice(&profile.name);
                        path.extend_from_slice(b".utf");
                        if archive_exists(&path) {
                            profile.utf_path = Some(path);
                        }
                        current = Some(id);
                    } else {
                        current = None;
                    }
                } else {
                    let id = create_id(&extend);
                    current = self.profiles.contains_key(&id).then_some(id);
                }
            } else if reader.is_header(b"Sound") || reader.is_header(b"VoiceSound") {
                let Some(profile) = current.and_then(|id| self.profiles.get_mut(&id)) else {
                    continue;
                };
                let mut msg = VoiceMsg::new();
                while reader.read_value() {
                    if reader.is_value(b"file") {
                        profile.set_sound_file(msg.id, reader.value_string());
                    } else if reader.is_value(b"priority") {
                        msg.priority = reader.value_int(0).to_le_bytes()[0].cast_signed();
                    } else if reader.is_value(b"attenuation") {
                        msg.attenuation = reader.value_int(0).to_le_bytes()[0].cast_signed();
                    } else if reader.is_value(b"msg") {
                        msg.id = create_id(&reader.value_string_at(0));
                    }
                }
                if msg.id != 0 {
                    profile.messages.entry(msg.id).or_insert(msg);
                }
            }
        }
        self.profiles.len() - before
    }

    /// `VoiceMsg::Acquire`: the mixer archetype on first use (the wav read
    /// out of the profile's archive), then the use counted; true when there
    /// is a mixer archetype.
    pub fn acquire(&mut self, profile: u32, msg: u32, wavs: &mut dyn WavFactory) -> bool {
        let Some(p) = self.profiles.get_mut(&profile) else {
            return false;
        };
        let file = p.sound_file(msg);
        let archive = p.utf_path.clone();
        let Some(m) = p.messages.get_mut(&msg) else {
            return false;
        };
        if m.sm_arch_id == -1 {
            m.sm_arch_id = wavs.create_wav(&WavRequest {
                file: Some(&file),
                archive: archive.as_deref(),
                is_2d: true,
                pitch_bendable: false,
            });
        }
        m.ref_count = m.ref_count.wrapping_add(1);
        m.sm_arch_id != -1
    }

    /// `VoiceMsg::Release`: a use dropped; below one the mixer archetype is
    /// destroyed. The remaining count.
    pub fn release(&mut self, profile: u32, msg: u32, wavs: &mut dyn WavFactory) -> i32 {
        let Some(m) = self
            .profiles
            .get_mut(&profile)
            .and_then(|p| p.messages.get_mut(&msg))
        else {
            return 0;
        };
        if m.ref_count != 0 {
            m.ref_count -= 1;
        }
        if m.ref_count < 1 && m.sm_arch_id != -1 {
            wavs.destroy_wav(m.sm_arch_id);
            m.sm_arch_id = -1;
        }
        i32::from(m.ref_count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const INI: &[u8] = b"[Voice]\nnickname = pilot\nscript = a\nscript = b\n\
        [Sound]\nmsg = hello\nattenuation = -6\npriority = 3\n\
        [Sound]\nmsg = hello\nattenuation = -1\n\
        [Voice]\nnickname = plain\n\
        [Sound]\nfile = x.wav\nmsg = late\n\
        [Sound]\nmsg = early\nfile = e.wav\n\
        [Voice]\nextend = pilot\n\
        [Sound]\nmsg = bye\n";

    #[test]
    fn profiles_lines_and_their_files() {
        let mut t = VoiceTable::new();
        let added = t.load_ini(INI, &|p: &[u8]| p == b"audio\\pilot.utf");
        assert_eq!(added, 2);
        let pilot = t.get(create_id(b"pilot")).unwrap();
        assert_eq!(pilot.scripts.len(), 2);
        let hello = create_id(b"hello");
        // The first line of an id wins.
        assert_eq!(pilot.messages[&hello].attenuation, -6);
        assert_eq!(pilot.messages[&hello].priority, 3);
        assert!(pilot.messages.contains_key(&create_id(b"bye")));
        assert_eq!(pilot.sound_file(hello), format!("0x{hello:X}").into_bytes());
        let plain = t.get(create_id(b"plain")).unwrap();
        // `file =` before `msg =` lands on id 0, as in the reference.
        assert_eq!(plain.sound_file(create_id(b"late")), b"");
        assert_eq!(plain.sound_file(create_id(b"early")), b"e.wav");
    }
}
