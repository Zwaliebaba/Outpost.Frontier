//! The mission dialogs (TU28, `Content/MissionDialogs.cpp`): a `[Dialog]`
//! section of a mission file becomes one [`MissionDialog`] in Content's
//! `GMissionDialogs` ([`ContentState::mission_dialogs`]) under `CreateID`
//! of its nickname; a second dialog of the same id keeps the first. The
//! script mission's end empties the map (P3.5c); its destructor does not.
//!
//! `Act_StartDialog` starts one ([`start_mission_dialog`], P3.5c4): every
//! speaker gets its lines as pending comms
//! ([`crate::content::systems::PendingComm`]), each said when the line
//! before it ends (SpaceSupport's `CommComplete`,
//! [`crate::content::systems::send_pending_comms`]), and the first line is
//! said at once.
//!
//! [`ContentState::mission_dialogs`]: crate::content::ContentState::mission_dialogs

use std::collections::BTreeMap;

use neuron_core::ini::IniReader;
use neuron_server::controller::ControllerId;

use super::field::record;
use super::{nickname_id, system_id_into, value_tstring};
use crate::content::gcs::{self, SpaceObjRef};
use crate::content::players;
use crate::content::ships::ShipKey;
use crate::content::systems::{self, PendingComm};
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

/// `"Player"`'s object for a dialog: the ship of the player whose ship has
/// the id `FindObjectIdByName` gave, else of the dialog's player
/// (`FindContentPlayerStateByShip`, then `FindContentPlayerState`); `None`
/// without such a player state, `Some(None)` for a player without a ship.
fn player_object(cx: &Cx<'_>, found: u32, player: u32) -> Option<Option<u32>> {
    let states = &cx.logic.content.state.players;
    let owner = players::find_by_ship(states, found)
        .or_else(|| states.contains_key(&player).then_some(player))?;
    Some(
        states
            .get(&owner)
            .and_then(|s| s.ship.as_ref())
            .map(|_| owner),
    )
}

/// `"Player"`'s object as a system object (`ShipObject`), or none.
fn player_ship_ref(cx: &Cx<'_>, found: u32, player: u32) -> Option<SpaceObjRef> {
    player_object(cx, found, player)
        .flatten()
        .map(|p| SpaceObjRef::Ship(ShipKey::Player(p)))
}

/// `FMissionDialog::Start(&PlayerId)` (`MissionDialogs.cpp:78-213`): false
/// when the player is in another system than the dialog's; nothing (true)
/// without the dialog's system. Otherwise each speaker in name order: each
/// of its lines' targets is resolved (`FindObjectIdByName` with the
/// dialog's mission; `Player` the ship of the player of that id, else of
/// `player`; any other name a ship or solar of the record) and written into
/// the line, a first line also resolving its speaker; every line goes into
/// the speaker's new pending comms, with its target as it now is. The last
/// first line found is then said from its speaker (1 s when its target is
/// a player's ship, else 0.5 s). False when a speaker or target was not
/// found.
fn start(cx: &mut Cx<'_>, dialog: &mut MissionDialog, player: u32) -> bool {
    let mut result = true;
    if let Some(state) = cx.logic.content.state.players.get(&player)
        && let (Some(at), Some(wanted)) = (state.system, dialog.system)
        && at != wanted
    {
        return false;
    }
    let Some(system) = dialog.system else {
        return result;
    };
    let controller = dialog.controller.0;
    let mut speaker: Option<SpaceObjRef> = None;
    let (mut target_id, mut text_id) = (0u32, 0u32);
    for (name, lines) in &mut dialog.speakers {
        let mut comms = Vec::with_capacity(lines.len());
        for line in lines.iter_mut() {
            let mut line_target =
                systems::find_object_id_by_name(cx, system, &line.target, controller);
            let found = if line.target == b"Player" {
                match player_object(cx, line_target, player).flatten() {
                    Some(owner) => {
                        line_target = cx.logic.content.state.players[&owner]
                            .ship
                            .as_ref()
                            .map_or(0, |s| s.ship);
                        true
                    }
                    None => false,
                }
            } else {
                gcs::find_ship_or_solar(cx, Some(system), line_target).is_some()
            };
            if found {
                line.target_id = line_target;
                if line.first {
                    let speaker_id =
                        systems::find_object_id_by_name(cx, system, &line.speaker, controller);
                    if line.speaker == b"Player" {
                        match player_object(cx, speaker_id, player) {
                            // No player state: the speaker is kept.
                            None => result = false,
                            Some(_) => {
                                speaker = player_ship_ref(cx, speaker_id, player);
                                if speaker.is_none() {
                                    result = false;
                                }
                            }
                        }
                    } else {
                        speaker = gcs::find_ship_or_solar(cx, Some(system), speaker_id);
                        if speaker.is_none() {
                            result = false;
                        }
                    }
                    target_id = line.target_id;
                    text_id = line.text_id;
                }
            } else {
                result = false;
            }
            comms.push(PendingComm {
                key: line.key,
                text_id: line.text_id,
                target_id: line.target_id,
                field_0x0c: line.field_0x0c,
            });
        }
        let owner_id = systems::find_object_id_by_name(cx, system, name, controller);
        let owner = if name.as_slice() == b"Player" {
            player_ship_ref(cx, owner_id, player)
        } else {
            gcs::find_ship_or_solar(cx, Some(system), owner_id)
        };
        match owner {
            Some(owner) => systems::set_pending_comms(cx, owner, comms),
            None => result = false,
        }
    }
    let Some(speaker) = speaker else {
        return result;
    };
    // The first player of the map whose ship (0 without one) is the target.
    let duration = if cx
        .logic
        .content
        .state
        .players
        .values()
        .any(|p| p.ship.as_ref().map_or(0, |s| s.ship) == target_id)
    {
        1.0
    } else {
        0.5
    };
    let sender = systems::space_obj_id(cx, speaker);
    let _ = systems::send_comm(cx, sender, target_id, text_id, None, duration);
    result
}

/// `StartMissionDialog(&DialogId, &PlayerId)` (`MissionDialogs.cpp:216-223`,
/// `Act_StartDialog`'s): [`start`] of the dialog of the id in
/// `GMissionDialogs`, false when there is none. The lines' targets stay
/// written into the map's dialog, as in the C++.
pub fn start_mission_dialog(cx: &mut Cx<'_>, dialog_id: u32, player: u32) -> bool {
    let Some(mut dialog) = cx
        .logic
        .content
        .state
        .mission_dialogs
        .get(&dialog_id)
        .cloned()
    else {
        return false;
    };
    // Nothing `start` calls reaches the dialog map (no message is sent).
    let result = start(cx, &mut dialog, player);
    if let Some(slot) = cx.logic.content.state.mission_dialogs.get_mut(&dialog_id) {
        *slot = dialog;
    }
    result
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
