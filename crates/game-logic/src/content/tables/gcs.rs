//! The GCS exclusions of `scripts\GCS\GCSExcl.ini` (TU34,
//! `Content/GenericScripts.cpp:727-768`): the scripts a voice must not play.

use std::collections::BTreeMap;

use neuron_core::ini::IniReader;

use super::voice_id;

/// `LoadGcsExclusions`: each `script` line appended to the list of the
/// `voice` before it. The C++ opens the file itself and returns at once when
/// it does not open.
///
/// The voice id is one slot for the whole load, written only by
/// `pub::GetVoiceID`, so an empty `voice` reuses the voice before; the C++
/// leaves it unset before the first voice line and the port starts at 0
/// (D781). A `script` before any `voice` writes through the map's end in the
/// C++; the port ignores it (D780).
pub fn load_gcs_exclusions(table: &mut BTreeMap<u32, Vec<Vec<u8>>>, reader: &mut IniReader) {
    let mut voice = 0;
    while reader.read_header() {
        if !reader.is_header(b"GCS_Exclusions") {
            continue;
        }
        let mut current: Option<u32> = None;
        while reader.read_value() {
            if reader.is_value(b"voice") {
                voice_id(&mut voice, &reader.value_string());
                table.entry(voice).or_default();
                current = Some(voice);
            } else if reader.is_value(b"script") {
                let script = neuron_core::hash::c_str(&reader.value_string()).to_vec();
                if let Some(list) = current.and_then(|v| table.get_mut(&v)) {
                    list.push(script);
                }
            }
        }
    }
}
