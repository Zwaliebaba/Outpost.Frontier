//! The voice-line durations (TU53, `Content/Voices.cpp`) and the voice
//! properties (TU63, `RandomMissionGen.cpp:1688-1802`).

use std::collections::BTreeMap;

use game_sim::data::vc6::vc6_sort;
use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::voice_id;

/// One `[Sound]` line (`FVoiceLine`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct VoiceLine {
    /// The `[Voice]` nickname's id before it, kept across sections.
    pub voice: u32,
    /// `msg`, `pub::GetNicknameId`.
    pub msg: u32,
    /// `duration`.
    pub duration: f32,
}

/// `VoiceLineLess`: by voice, then message, both unsigned.
fn line_less(a: &VoiceLine, b: &VoiceLine) -> bool {
    (a.voice, a.msg) < (b.voice, b.msg)
}

/// `LoadVoiceLines`: appends the file's `[Sound]` lines and sorts the whole
/// table with the VC6 `std::sort`, whose order of equal keys is the C++'s.
/// The C++ sorts after every file, opened or not, and so does the port.
pub fn load_voice_lines(table: &mut Vec<VoiceLine>, reader: &mut IniReader) {
    let mut voice = 0;
    while reader.read_header() {
        if reader.is_header(b"Voice") {
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    voice_id(&mut voice, &reader.value_string());
                }
            }
        } else if reader.is_header(b"Sound") {
            let mut msg = 0;
            let mut duration = -1.0_f64;
            while reader.read_value() {
                if reader.is_value(b"msg") {
                    msg = create_id(&reader.value_string());
                } else if reader.is_value(b"duration") {
                    duration = reader.value_float(0);
                }
            }
            // The C++ inserts unless the unrounded value equals -1.0; NaN
            // inserts too.
            if duration != -1.0 {
                table.push(VoiceLine {
                    voice,
                    msg,
                    duration: duration as f32,
                });
            }
        }
    }
    vc6_sort(table, line_less);
}

/// `FindVoiceLineDuration`: the duration of a voice's message, -1 for none.
#[must_use]
pub fn voice_line_duration(table: &[VoiceLine], voice: u32, msg: u32) -> f32 {
    let at = table.partition_point(|l| (l.voice, l.msg) < (voice, msg));
    match table.get(at) {
        Some(l) if l.voice == voice && l.msg == msg => l.duration,
        _ => -1.0,
    }
}

/// One `[mVoiceProp]` record (`FVoiceProperties`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VoiceProperties {
    /// `voice`, `pub::GetVoiceID`.
    pub voice: u32,
    /// `gender` and `supports_roles` (male 1, female 2, `combat_lev_1`
    /// 0x4000, `combat_lev_2` 4, `combat_lev_3` 0xc004, `scan` 8 and so on).
    pub flags: u32,
    /// `permutation_count`: nickname id to count, the first kept.
    pub permutation_counts: BTreeMap<u32, i32>,
}

impl VoiceProperties {
    /// `GetPermutationCount`: 0 for a nickname without one.
    #[must_use]
    pub fn permutation_count(&self, nickname: u32) -> i32 {
        self.permutation_counts.get(&nickname).copied().unwrap_or(0)
    }

    /// `SupportsRoles`: every bit of `mask` set.
    #[must_use]
    pub fn supports_roles(&self, mask: u32) -> bool {
        self.flags & mask == mask
    }
}

/// The `supports_roles` names and their bits, compared case-sensitively.
const ROLES: [(&[u8], u32); 14] = [
    (b"combat_lev_1", 0x4000),
    (b"combat_lev_2", 0x4),
    (b"combat_lev_3", 0xc004),
    (b"scan", 0x8),
    (b"hail_hailee", 0x10),
    (b"hail_dock", 0x20),
    (b"dockrequest_dock", 0x40),
    (b"dockrequest_docker", 0x80),
    (b"behavior", 0x100),
    (b"misc_chatter", 0x200),
    (b"fidget", 0x400),
    (b"combat_dock", 0x800),
    (b"random_mission_ship", 0x1000),
    (b"mission_commission", 0x2000),
];

/// Where the section's record is: an existing element or a new one.
enum Target {
    None,
    Existing(usize),
    Fresh(Box<VoiceProperties>),
}

impl Target {
    fn record<'t>(
        &'t mut self,
        table: &'t mut [VoiceProperties],
    ) -> Option<&'t mut VoiceProperties> {
        match self {
            Self::None => None,
            Self::Existing(at) => table.get_mut(*at),
            Self::Fresh(record) => Some(record),
        }
    }
}

/// `LoadVoiceProperties`: each `[mVoiceProp]` adds to the record of an
/// existing voice or appends a new one at the end of the section.
///
/// The voice id outlives the sections, as the C++'s frame slot does. A key
/// before the section's `voice` writes through a null record in the C++;
/// the port ignores it (D780).
pub fn load_voice_properties(table: &mut Vec<VoiceProperties>, reader: &mut IniReader) {
    let mut voice = 0;
    while reader.read_header() {
        if !reader.is_header(b"mVoiceProp") {
            continue;
        }
        let mut target = Target::None;
        let mut existing = false;
        while reader.read_value() {
            if reader.is_value(b"voice") {
                voice_id(&mut voice, &reader.value_string());
                if let Some(at) = table.iter().position(|p| p.voice == voice) {
                    existing = true;
                    target = Target::Existing(at);
                } else {
                    target = Target::Fresh(Box::default());
                }
                if let Some(record) = target.record(table) {
                    record.voice = voice;
                }
            } else if reader.is_value(b"gender") {
                let value = reader.value_string();
                let Some(record) = target.record(table) else {
                    continue;
                };
                // `_stricmp` for "male", then a case-sensitive compare for
                // "female".
                if eq_ignore_case(&value, b"male") {
                    record.flags |= 0x1;
                } else if value == b"female" {
                    record.flags |= 0x2;
                }
            } else if reader.is_value(b"supports_roles") {
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    let role = reader.value_string_at(index);
                    if let Some(record) = target.record(table)
                        && let Some((_, bits)) = ROLES.iter().find(|(name, _)| *name == role)
                    {
                        record.flags |= bits;
                    }
                    index += 1;
                }
            } else if reader.is_value(b"permutation_count") {
                let key = create_id(&reader.value_string_at(0));
                let count = reader.value_int(1);
                if let Some(record) = target.record(table) {
                    record.permutation_counts.entry(key).or_insert(count);
                }
            }
        }
        if !existing && let Target::Fresh(record) = target {
            table.push(*record);
        }
    }
}

/// `FindVoiceProperties`: the first record of a voice.
#[must_use]
pub fn find_voice_properties(table: &[VoiceProperties], voice: u32) -> Option<&VoiceProperties> {
    table.iter().find(|p| p.voice == voice)
}
