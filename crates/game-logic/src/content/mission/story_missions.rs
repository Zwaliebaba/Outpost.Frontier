//! The story missions' overrides (`Content/StoryMissions.cpp`, TU0 to TU14;
//! work package P3.5d): the slots `Mission_01a`, `Mission_01b` and
//! `Mission_13` replace in the script mission's table. `Mission_02` to
//! `Mission_12` and `GenericMission` change only their constructors
//! ([`super::construct_script`]).
//!
//! - `FMission01a::HandleMissionMessage` (`StoryMissions.cpp:108-174`): the
//!   first `BaseEnter` of `He01_01_Base` applies the desired state again
//!   and marks the mission, whose single-player save then adds
//!   `[Mission01aSave]`; its restore reads the state back.
//! - `FMission01b::HandleMissionMessage` (`:207-275`) and
//!   `LoadSuccessState` (`:277-303`): the first `BaseEnter` (or a restore)
//!   joins the player's group to the mission, reports the acceptance to
//!   Story, sets the attack clamp and starts `He01_02`'s cut scene (a
//!   restore finishes the success scene instead); the single-player save
//!   adds `[Mission01bSave]`.
//! - `FMission13::OnMissionSucceed` (`:443-452`): after the script's
//!   success, mad lib `0x5faa` shown with type 9 ([`on_mission_succeed`]).
//!
//! The C++ reads the player through the mission's cached `PlayerState`
//! and calls Story through `StoryEvents` without testing either; the port
//! does nothing without them (D1046).

use std::io::Write as _;

use game_sim::protocol::fmtstr::FmtStr;
use neuron_server::controller::ControllerId;

use super::runtime::{player_state, script, script_mut};
use super::save::RtcScript;
use super::{base_id_of, engine, objectives, scenes, tstring};
use crate::api;
use crate::content::players;
use crate::events::ControllerMessage;
use crate::server::{ControllerKind, Cx};

/// `Mission_01b`'s cut scene on `He01_02_Base` (`StoryMissions.cpp:233`).
pub const M01B_BASE_RTC: &[u8] = b"missions\\m01a\\M001a_s006x_He01_02_nrml.ini";
/// The success scene a restored `Mission_01b` finishes (`:291`).
pub const M01B_SUCCESS_RTC: &[u8] = b"missions\\m01a\\rtc_success.ini";
/// `Mission_13`'s success mad lib (`:448`).
pub const M13_SUCCESS_MAD_LIB: u32 = 0x5faa;

/// What the overrides need of a message, copied out of it.
#[derive(Clone, Copy)]
enum Case {
    BaseEnter { base: u32, player: u32 },
    Save { client: u32 },
    Load { player: u32 },
    Other,
}

fn case(message: &ControllerMessage<'_>) -> Case {
    match *message {
        ControllerMessage::BaseEnter { base, player } => Case::BaseEnter { base, player },
        ControllerMessage::PlayerSave { client, .. } => Case::Save { client },
        ControllerMessage::PlayerLoad { player, .. } => Case::Load { player },
        _ => Case::Other,
    }
}

/// Slot 21, `HandleMissionMessage`, of the mission's kind: the override of
/// `Mission_01a` or `Mission_01b`, else the script's
/// ([`engine::handle_mission_message`]).
pub fn handle_mission_message(
    cx: &mut Cx<'_>,
    id: ControllerId,
    message: &mut ControllerMessage<'_>,
) {
    match script(cx, id).map(|s| s.kind) {
        Some(ControllerKind::Mission01a) => mission_01a(cx, id, message),
        Some(ControllerKind::Mission01b) => mission_01b(cx, id, message),
        _ => engine::handle_mission_message(cx, id, message),
    }
}

/// `PlayerState->PlayerId`: the mission's player while he has a state
/// (D1046).
fn cached_player(cx: &Cx<'_>, id: ControllerId) -> Option<u32> {
    let s = script(cx, id)?;
    player_state(cx, s).map(|_| s.object.player)
}

/// Story's `OnMissionEvent(kind, CampaignSlotSucceed, ControllerId)`, when
/// the mission has a Story (D1046 for the C++'s untested calls).
pub fn notify_story(cx: &mut Cx<'_>, id: ControllerId, kind: u32) {
    let Some(s) = script(cx, id) else { return };
    let (story, chapter) = (s.story, s.campaign_slot_succeed as u32);
    crate::content::story::on_mission_event(cx, story, kind, chapter, id.0);
}

/// `fprintf(File, "\n[<header>]\nMissionStateNum = %d\n\n", previous)`
/// into the save of a `PlayerSave` message.
fn write_state(message: &mut ControllerMessage<'_>, header: &str, previous: i32) {
    if let ControllerMessage::PlayerSave { file, .. } = message {
        // The C++ does not test `fprintf`.
        let _ = write!(file, "\n[{header}]\nMissionStateNum = {previous}\n\n");
    }
}

/// `FMission01a::HandleMissionMessage` (`StoryMissions.cpp:108-174`).
///
/// - `BaseEnter {base, player}`: the script's case; then, for the player,
///   the first time the base is `He01_01_Base` (`GHe0101BaseId`), the mark
///   set and `ApplyDesiredMissionState` (slot 30).
/// - `PlayerSave {player, file}`: for the player in single player only, the
///   script's case, then with the mark `[Mission01aSave]` with the previous
///   state.
/// - `PlayerLoad {player, reader}`: for the player, the script's case, then
///   from the reader's start each `[Mission01aSave]` `MissionStateNum` as
///   the previous and desired state, renumbering every objective unless it
///   is 3; the reader's state is kept around the read.
fn mission_01a(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    match case(message) {
        Case::BaseEnter { base, player } => {
            engine::handle_mission_message(cx, id, message);
            if cached_player(cx, id) != Some(player) {
                return;
            }
            let Some(s) = script_mut(cx, id) else { return };
            let he0101 = s.story_fields.bases.map_or(0, |(he0101, _)| he0101);
            if base != he0101 || s.story_fields.base_enter_handled {
                return;
            }
            s.story_fields.base_enter_handled = true;
            engine::apply_desired_state(cx, id);
        }
        Case::Save { client } => {
            if cached_player(cx, id) != Some(client) || !api::misc::single_player(cx) {
                return;
            }
            engine::handle_mission_message(cx, id, message);
            let Some(s) = script(cx, id) else { return };
            if s.story_fields.base_enter_handled {
                let previous = s.previous_state;
                write_state(message, "Mission01aSave", previous);
            }
        }
        Case::Load { player } => {
            if cached_player(cx, id) != Some(player) {
                return;
            }
            engine::handle_mission_message(cx, id, message);
            let ControllerMessage::PlayerLoad { reader, .. } = message else {
                return;
            };
            let saved = reader.state();
            reader.reset();
            while reader.read_header() {
                if !reader.is_header(b"Mission01aSave") {
                    continue;
                }
                while reader.read_value() {
                    if !reader.is_value(b"MissionStateNum") {
                        continue;
                    }
                    let value = reader.value_int(0);
                    if let Some(s) = script_mut(cx, id) {
                        s.previous_state = value;
                        s.mission_state = value;
                    }
                    if value != 3 {
                        objectives::renumber_all(cx, id);
                    }
                }
            }
            reader.set_state(&saved);
        }
        Case::Other => engine::handle_mission_message(cx, id, message),
    }
}

/// The acceptance `Mission_01b` makes on its first `BaseEnter` and on a
/// restore (`StoryMissions.cpp:222-230`, `:280-287`): a cleared vnpc, the
/// player's group joined to the mission (no notice), Story's event 1 with
/// the mission's chapter, and the attack clamp set
/// (`SpacePopSetAttackClamp(1)`).
fn accept_01b(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    players::join_mission_as_group(cx, player, id.0, 0, 0, false, 0);
    notify_story(cx, id, 1);
    cx.logic.content.state.clamps.attack_clamp = 1;
}

/// `FMission01b::HandleMissionMessage` (`StoryMissions.cpp:207-275`).
///
/// - `BaseEnter {base, player}`: the script's case; then, for the player,
///   once: `pub::GetBaseID("He01_02_base")` (not used), the acceptance
///   ([`accept_01b`]), the mark, and the cut scene of `He01_02_Base`
///   started and appended to the cut scenes.
/// - `PlayerSave {player, file}`: for the player in single player only, the
///   script's case, then `[Mission01bSave]` with the previous state.
/// - `PlayerLoad {player, reader}`: for the player, the script's case, then
///   from the reader's start `LoadSuccessState` (the reader's state kept
///   around it) and the mark.
fn mission_01b(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    match case(message) {
        Case::BaseEnter { player, .. } => {
            engine::handle_mission_message(cx, id, message);
            let Some(own) = cached_player(cx, id) else {
                return;
            };
            if own != player || script(cx, id).is_none_or(|s| s.story_fields.base_enter_handled) {
                return;
            }
            let _ = base_id_of(cx, b"He01_02_base");
            accept_01b(cx, id, own);
            if let Some(s) = script_mut(cx, id) {
                s.story_fields.base_enter_handled = true;
            }
            // A node {path, no scene, no script, not finished, not
            // repeatable}, started, then appended.
            let mut rtc = RtcScript {
                path: tstring(M01B_BASE_RTC, 0x80),
                handle: -1,
                ..RtcScript::default()
            };
            scenes::start_rtc_script(cx, id, &mut rtc);
            if let Some(s) = script_mut(cx, id) {
                s.rtc_scripts.push(rtc);
            }
        }
        Case::Save { client } => {
            if cached_player(cx, id) != Some(client) || !api::misc::single_player(cx) {
                return;
            }
            engine::handle_mission_message(cx, id, message);
            let previous = script(cx, id).map_or(0, |s| s.previous_state);
            write_state(message, "Mission01bSave", previous);
        }
        Case::Load { player } => {
            let Some(own) = cached_player(cx, id).filter(|&own| own == player) else {
                return;
            };
            engine::handle_mission_message(cx, id, message);
            let ControllerMessage::PlayerLoad { reader, .. } = message else {
                return;
            };
            let saved = reader.state();
            reader.reset();
            load_success_state(cx, id, own, reader);
            reader.set_state(&saved);
            if let Some(s) = script_mut(cx, id) {
                s.story_fields.base_enter_handled = true;
            }
        }
        Case::Other => engine::handle_mission_message(cx, id, message),
    }
}

/// `FMission01b::LoadSuccessState(Reader)` (`StoryMissions.cpp:277-303`):
/// the mark, the acceptance ([`accept_01b`]), the success scene finished
/// (`RemoveMissionRtc`), then each `[Mission01bSave]` `MissionStateNum` as
/// the desired state.
fn load_success_state(
    cx: &mut Cx<'_>,
    id: ControllerId,
    player: u32,
    reader: &mut neuron_core::ini::IniReader,
) {
    if let Some(s) = script_mut(cx, id) {
        s.story_fields.base_enter_handled = true;
    }
    accept_01b(cx, id, player);
    scenes::remove_mission_rtc(cx, id, &tstring(M01B_SUCCESS_RTC, 0x80));
    while reader.read_header() {
        if !reader.is_header(b"Mission01bSave") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"MissionStateNum") {
                let value = reader.value_int(0);
                if let Some(s) = script_mut(cx, id) {
                    s.mission_state = value;
                }
            }
        }
    }
}

/// Slot 23, `OnMissionSucceed`, of the mission's kind: `Mission_13`'s
/// override (`StoryMissions.cpp:443-452`) runs the script's success, then
/// shows mad lib `0x5faa` to the player it had before (type 9, to the
/// group); every other kind the script's alone.
pub fn on_mission_succeed(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let (kind, player) = (s.kind, s.object.player);
    engine::on_mission_succeed(cx, id);
    if kind == ControllerKind::Mission(13) {
        let text = FmtStr::new(M13_SUCCESS_MAD_LIB);
        let _ = api::player::display_mission_message(cx, player, &text, 9, true);
    }
}
