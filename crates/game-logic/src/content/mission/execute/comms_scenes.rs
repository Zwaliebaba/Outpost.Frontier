//! P3.5c4's actions (`Content/MissionAct.cpp`): the comms, the cameras and
//! scene scripts, the cut scenes, the ambient scenes, the pop-up and the
//! mission's offer and title. `Act_StartDialog`'s dialog
//! ([`super::super::dialog::start_mission_dialog`]) runs from its delay's
//! arm in [`super::execute`].
//!
//! The comms go through the system's `SendComm` and `pub::SpaceObj::SendComm`
//! (P3.6b's [`crate::replication::send_comm`]); the cameras, scene scripts,
//! pop-ups and mission messages are the `pub::Player` and
//! `pub::SpaceObj` calls that send to the player's client.

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_server::controller::ControllerId;

use super::super::action::Action;
use super::super::runtime::{ID_MASK, label_object_id, label_ship_id, player_state, script};
use super::super::save::RtcScript;
use super::super::scenes;
use crate::api;
use crate::api::space_obj::CommRequest;
use crate::content::systems::{self, CommSpeaker};
use crate::server::Cx;

/// A comm's duration: the action's seconds when set, 1 s for a receiver
/// that is the player's ship with no id (`ReceiverId == GetShipId() &&
/// (ReceiverId & 0x3fffffff) == 0`), else 0.5 s.
fn comm_duration(duration: i32, receiver: u32, player_ship: Option<u32>) -> f32 {
    if duration != -1 {
        return duration as f32;
    }
    if player_ship == Some(receiver) && receiver & ID_MASK == 0 {
        1.0
    } else {
        0.5
    }
}

/// The player's ship id (`GetShipId`, 0 without a ship) when the player
/// has a state.
fn player_ship(cx: &Cx<'_>, id: ControllerId) -> Option<u32> {
    let s = script(cx, id)?;
    player_state(cx, s).map(|p| p.ship.as_ref().map_or(0, |o| o.ship))
}

/// `Act::Execute` of the P3.5c4 action classes.
#[allow(clippy::too_many_lines)]
pub fn execute(cx: &mut Cx<'_>, id: ControllerId, action: &mut Action) -> bool {
    let Some(s) = script(cx, id) else { return true };
    let player = s.object.player;
    match action {
        Action::SendComm(a) => {
            // `MissionAct.cpp:3195-3234`.
            let sender = label_object_id(cx, s, &a.sender);
            let anyone = eq_ignore_case(c_str(&a.receiver), b"anyone");
            let receiver = label_object_id(cx, s, &a.receiver);
            let sender_set = sender & ID_MASK != 0;
            let receiver_set = receiver & ID_MASK != 0 || anyone;
            let result = sender_set && receiver_set;
            let Some(state) = player_state(cx, s) else {
                return result;
            };
            if !result {
                return result;
            }
            if state.system.is_none() {
                return result;
            }
            let ship = state.ship.as_ref().map_or(0, |o| o.ship);
            let duration = comm_duration(a.duration, receiver, Some(ship));
            // A comm with its own head carries the action's voice, comm
            // type and costume; any other none.
            let speaker = (a.speaker_costume.part_ids[0] != 0).then(|| CommSpeaker {
                voice: a.voice_id,
                comm_type: a.comm_type,
                costume: a.speaker_costume.clone(),
            });
            let _ = systems::send_comm(cx, sender, receiver, a.text_id, speaker, duration);
            result
        }
        Action::EtherComm(a) => {
            // `MissionAct.cpp:3289-3306`.
            let receiver = label_ship_id(cx, s, &a.receiver);
            let duration = comm_duration(a.duration, receiver, player_ship(cx, id));
            let request = CommRequest {
                sender: 0,
                target: receiver,
                voice: a.voice_id,
                costume: Some(a.speaker_costume.clone()),
                comm_type: a.comm_type,
                text_ids: vec![a.text_id],
                mood: 0,
                duration,
                interrupt: true,
            };
            let _ = api::space_obj::send_comm(cx, &request);
            true
        }
        Action::CallSceneScript(a) => {
            // `MissionAct.cpp:2164-2188`.
            let target = label_object_id(cx, s, &a.target);
            if target & ID_MASK == 0 {
                return false;
            }
            let _ = api::space_obj::request_space_script(
                cx, target, a.position, a.mode, player, &a.script,
            );
            scenes::enter_static_scene(cx, id);
            true
        }
        Action::StaticCam(a) => {
            // `MissionAct.cpp:3680-3700`.
            if api::player::set_camera(cx, player, &a.camera, a.fov, a.aspect).is_ok() {
                scenes::enter_static_scene(cx, id);
            }
            true
        }
        Action::RevertCam(_) => {
            // `MissionAct.cpp:622-627`.
            if api::player::revert_camera(cx, player).is_ok() {
                scenes::revert_static_cam(cx, id);
            }
            true
        }
        Action::AddRtc(a) => {
            // `MissionAct.cpp:2494-2503`: a record {path, no scene, no
            // script, not finished, repeatable}, started, then appended.
            let mut record = RtcScript {
                path: a.path.clone(),
                handle: -1,
                field_0x95: a.repeatable,
                ..RtcScript::default()
            };
            scenes::start_rtc_script(cx, id, &mut record);
            if let Some(s) = super::super::runtime::script_mut(cx, id) {
                s.rtc_scripts.push(record);
            }
            true
        }
        Action::RemoveRtc(a) => {
            // `MissionAct.cpp:951-955`.
            scenes::remove_mission_rtc(cx, id, &a.rtc_file);
            true
        }
        Action::AddAmbient(a) => {
            // `MissionAct.cpp:2422-2436`.
            a.handle = scenes::add_ambient(cx, id, &a.path, a.room_id, a.base_id);
            true
        }
        Action::RemoveAmbient(a) => {
            // `MissionAct.cpp:2455-2470`.
            scenes::remove_ambient(cx, id, &a.path);
            true
        }
        Action::PopUpDialog(a) => {
            // `MissionAct.cpp:2599-2609`.
            let title = FmtStr::new(a.title_id);
            let text = FmtStr::new(a.text_id);
            api::player::pop_up_dialog(cx, player, &title, &text, a.buttons);
            true
        }
        Action::SetOffer(a) => {
            // `MissionAct.cpp:574-582`: the offer's mad lib, shown (type 8)
            // unless it is 0 or 1, then the objectives renumbered.
            let offer = a.offer_id;
            if let Some(s) = super::super::runtime::script_mut(cx, id) {
                s.object.offer = offer;
            }
            if offer != 1 && offer != 0 {
                let _ =
                    api::player::display_mission_message(cx, player, &FmtStr::new(offer), 8, true);
            }
            super::super::objectives::renumber_all(cx, id);
            true
        }
        Action::SetTitle(a) => {
            // `MissionAct.cpp:598-603`: the title's mad lib, then the
            // objectives renumbered.
            if let Some(s) = super::super::runtime::script_mut(cx, id) {
                s.object.title = a.title_id;
            }
            super::super::objectives::renumber_all(cx, id);
            true
        }
        _ => true,
    }
}
