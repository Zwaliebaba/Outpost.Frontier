//! The mission dialogs (TU28, `Content/MissionDialogs.cpp`): a `[Dialog]`
//! section of a mission file becomes one [`MissionDialog`] in Content's
//! `GMissionDialogs` ([`ContentState::mission_dialogs`]) under `CreateID`
//! of its nickname; a second dialog of the same id keeps the first. The
//! script mission's end empties the map (P3.5c); its destructor does not.
//!
//! [`ContentState::mission_dialogs`]: crate::content::ContentState::mission_dialogs

use std::collections::BTreeMap;

use neuron_core::ini::IniReader;
use neuron_server::controller::ControllerId;

use super::field::record;
use super::{nickname_id, system_id_into, value_tstring};
use crate::server::Cx;

record! {
    /// One `line = speaker, target, line` (`FMissionDialogLine`,
    /// `MissionDialogs.h:24`).
    DialogLine {
        /// Value 0, the speaker's key.
        speaker: Vec<u8> = Vec::new(),
        /// Value 1 (empty when absent).
        target: Vec<u8> = Vec::new(),
        /// The section's first line: the line id before it has none of its
        /// low 30 bits.
        first: bool = false,
        /// +0x69 (0).
        field_0x69: u8 = 0,
        /// The pending comm's key: the line id before this one (0 for the
        /// first).
        key: u32 = 0,
        /// `pub::GetNicknameId` of value 2.
        text_id: u32 = 0,
        /// The target's object (set when the dialog starts).
        target_id: u32 = 0,
        /// The pending comm's +0x0c (0).
        field_0x0c: u32 = 0,
    }
}

/// One dialog (`FMissionDialog`, `MissionDialogs.h:51`).
#[derive(Clone, Debug, PartialEq)]
pub struct MissionDialog {
    /// `ControllerId`: the mission's.
    pub controller: ControllerId,
    /// `Speakers`: each speaker's lines in section order, by name in
    /// `strcmp` order.
    pub speakers: BTreeMap<Vec<u8>, Vec<DialogLine>>,
    /// `System`: the system record of `system` (its id), none when absent
    /// or unknown.
    pub system: Option<u32>,
}

impl MissionDialog {
    /// A dialog of the mission `controller` with no speakers and no system.
    #[must_use]
    pub fn new(controller: ControllerId) -> Self {
        Self {
            controller,
            speakers: BTreeMap::new(),
            system: None,
        }
    }
}

/// `FMissionDialog::Load` (`MissionDialogs.cpp:21-73`): the rest of a
/// `[Dialog]` section. Each `line` goes to its speaker's lines (the
/// speaker added on first use), keyed on the line before it in the
/// section; `system` sets the system record (none for an unknown system:
/// the C++ looks up an unset local, D814).
pub fn load(cx: &mut Cx<'_>, reader: &mut IniReader, dialog: &mut MissionDialog) {
    let mut previous = 0u32;
    while reader.read_value() {
        if reader.is_value(b"line") {
            let speaker = value_tstring(reader, 0, 0x30);
            let text_id = nickname_id(&reader.value_string_at(2));
            let line = DialogLine {
                speaker: speaker.clone(),
                target: value_tstring(reader, 1, 0x30),
                first: previous & 0x3fff_ffff == 0,
                key: previous,
                text_id,
                ..DialogLine::default()
            };
            dialog.speakers.entry(speaker).or_default().push(line);
            previous = text_id;
        } else if reader.is_value(b"system") {
            let mut system = 0;
            dialog.system = if system_id_into(cx, &mut system, &reader.value_string_at(0)) {
                cx.logic
                    .content
                    .state
                    .systems
                    .contains_key(&system)
                    .then_some(system)
            } else {
                None
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::mission::field::Field;

    #[test]
    fn a_line_dumps_its_fields_in_order() {
        let line = DialogLine::default();
        let names: Vec<&str> = line.fields().iter().map(|(n, _)| *n).collect();
        assert_eq!(
            names,
            [
                "speaker",
                "target",
                "first",
                "field_0x69",
                "key",
                "text_id",
                "target_id",
                "field_0x0c"
            ]
        );
        assert_eq!(line.fields()[3].1, Field::U32(0));
    }
}
