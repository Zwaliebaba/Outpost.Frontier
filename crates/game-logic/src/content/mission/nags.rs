//! The nags (`FMissionNag`, `FMissionNagGreet` and `FMissionNagDist`,
//! `Content/MissionAct.cpp:1310-1803`; work package P3.5c5): the lines a
//! mission's speaker says to its player, by id in the script's `Nags` map.
//!
//! - A greeting ([`NagKind::Greet`], `Act_NagGreet`) hears `LaunchComplete`
//!   and greets the player's launched ship (`nag_greet_combat` or
//!   `nag_greet_normal`, by the speaker's combat flag).
//! - A distance nag ([`NagKind::Dist`], `Act_NagDistLeaving` and
//!   `Act_NagDistTowards`) arms once the player is farther than its distance
//!   from its point (a target object's position, refreshed each heartbeat),
//!   then every 20 s nags while the player came less than 900 closer and is
//!   still away (the target's kind picks the line); four nags without a
//!   speaker, or four of the speaker's lines finished, fail the mission
//!   with the nag's mad lib. Reaching the target (its gate, lane or dock)
//!   ends the nag.
//!
//! [`dispatch`] is `DispatchNagMessage` (`:1751-1800`), the script mission
//! handler's tail; [`say`] is `FMissionNag::Say` (`:1417-1458`). Each nag
//! timer takes the next of Content's unique ids and is set on the mission's
//! controller.
//!
//! The C++'s comparisons are kept as written: an x87 `FCOMP` branch is
//! false for a NaN on either side.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::data::costume::Costume;
use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use super::engine;
use super::runtime::{ID_MASK, script, script_mut};
use crate::api;
use crate::content::systems::{self, CommSpeaker};
use crate::events::{CommResult, ControllerMessage};
use crate::server::Cx;

/// The message ids a greeting hears (`LaunchComplete`).
const GREET_MESSAGES: [u32; 1] = [0x28];

/// The message ids a distance nag hears: `Heartbeat`, `CommComplete`,
/// `TimerExpired`, `TradelaneEnter`, `DockComplete`, `Msg0x27`.
const DIST_MESSAGES: [u32; 6] = [0x20, 0x0b, 0x3e, 0x16, 0x11, 0x27];

/// One nag (`FMissionNag`, `MissionAct.h`).
#[derive(Clone, Debug, PartialEq)]
pub struct Nag {
    /// `SpeakerId`: the speaker's object (0 for none).
    pub speaker: u32,
    /// `PlayerId`.
    pub player: u32,
    /// `SystemId`: the player's system when made, moved by `SystemEnter`.
    pub system: u32,
    /// `SpeakerCostume`: the speaker's `[NPC]` costume, `None` for none.
    pub costume: Option<Costume>,
    /// `VoiceId`.
    pub voice: u32,
    /// `LineId`: the last line said.
    pub line: u32,
    /// `SpeakerNameId`: the `[NPC]`'s individual name, else 1.
    pub speaker_name: u32,
    /// `bDone`: the nag leaves the map after the message.
    pub done: bool,
    /// The class.
    pub kind: NagKind,
}

/// A nag's class.
#[derive(Clone, Debug, PartialEq)]
pub enum NagKind {
    /// `FMissionNagGreet`.
    Greet,
    /// `FMissionNagDist`.
    Dist(NagDist),
}

/// `FMissionNagDist`'s own fields.
#[derive(Clone, Debug, PartialEq)]
pub struct NagDist {
    /// `TargetId`.
    pub target: u32,
    /// `Distance` (2000).
    pub distance: f32,
    /// `FailMadLibId` (1).
    pub fail_mad_lib: u32,
    /// `Position`.
    pub position: Vector,
    /// `LastPosition`: the player's position at the last check.
    pub last_position: Vector,
    /// `Threshold`: how much closer the player must come (900 armed).
    pub threshold: f32,
    /// `bPositionMode`: the position lines.
    pub position_mode: bool,
    /// `bNagInCombat`.
    pub nag_in_combat: bool,
    /// `bNagOutOfCombat` (true).
    pub nag_out_of_combat: bool,
    /// `bLeavingArea`: the leaving-area line.
    pub leaving_area: bool,
    /// `TimerId`.
    pub timer: u32,
    /// `bArmed`.
    pub armed: bool,
    /// `NagsLeft` (4).
    pub nags_left: i32,
}

impl Default for NagDist {
    /// The constructor's values (`MissionAct.cpp:1496-1518`).
    fn default() -> Self {
        Self {
            target: 0,
            distance: 2000.0,
            fail_mad_lib: 1,
            position: Vector::ZERO,
            last_position: Vector::ZERO,
            threshold: 0.0,
            position_mode: false,
            nag_in_combat: false,
            nag_out_of_combat: true,
            leaving_area: false,
            timer: 0,
            armed: false,
            nags_left: 4,
        }
    }
}

impl Nag {
    /// A nag of `kind` with the base part's values: no speaker, player or
    /// system, no costume, voice or line, name 0, not done.
    #[must_use]
    pub fn new(kind: NagKind) -> Self {
        Self {
            speaker: 0,
            player: 0,
            system: 0,
            costume: None,
            voice: 0,
            line: 0,
            speaker_name: 0,
            done: false,
            kind,
        }
    }

    /// Whether the nag hears message `event`.
    #[must_use]
    pub fn listens(&self, event: u32) -> bool {
        match self.kind {
            NagKind::Greet => GREET_MESSAGES.contains(&event),
            NagKind::Dist(_) => DIST_MESSAGES.contains(&event),
        }
    }
}

/// `FindNagSpeakerNpc` and `FillNagSpeaker` (`MissionAct.cpp:1319-1345`):
/// the player, the player's system (0 without a system record, which the
/// C++ reads through null, D1043), and the speaker's `[NPC]` record (the
/// mission ship slot named `speaker`, unless it is `none`): its costume,
/// voice and individual name, else none, 0 and 1. A slot whose `[NPC]` is
/// missing has none, as the reference takes `Act_NagDistTowards`' branch
/// for all three actions.
pub fn fill_speaker(cx: &Cx<'_>, id: ControllerId, nag: &mut Nag, speaker: &[u8]) {
    let Some(s) = script(cx, id) else { return };
    nag.player = s.object.player;
    nag.system = cx
        .logic
        .content
        .state
        .players
        .get(&s.object.player)
        .and_then(|p| p.system)
        .unwrap_or(0);
    let none = neuron_core::hash::eq_ignore_case(neuron_core::hash::c_str(speaker), b"none");
    let npc = if none {
        None
    } else {
        s.object
            .find_ship(speaker)
            .and_then(|slot| s.object.npcs.get(&slot.npc_id))
    };
    nag.costume = npc.map(|n| n.appearance.clone());
    nag.voice = npc.map_or(0, |n| n.voice);
    nag.speaker_name = npc.map_or(1, |n| n.individual_name as u32);
}

/// `FMissionNag::Say(name)` (`MissionAct.cpp:1417-1458`): nothing without
/// the player's state or while its ship is in behaviour 7. Without a
/// speaker, the NN line `<name>_01-`. Otherwise one of the speaker's voice
/// lines (`PickVoiceLine(name, "-", voice)`, none when it has none): the
/// speaker says it itself when it is in the player's system, else an
/// anonymous comm with the `[NPC]` voice, name and costume does (to the
/// player's ship, 0.5 s, only with a ship and a system record of the
/// nag's system).
pub fn say(cx: &mut Cx<'_>, nag: &mut Nag, name: &[u8]) {
    let Some(ship) = cx
        .logic
        .content
        .state
        .players
        .get(&nag.player)
        .map(crate::content::players::PlayerState::ship_id)
    else {
        return;
    };
    if ship & ID_MASK != 0 && api::ai::behavior_id(cx, ship) == 7 {
        return;
    }
    if nag.speaker & ID_MASK == 0 {
        let mut text = name.to_vec();
        text.extend_from_slice(b"_01-");
        nag.line = api::misc::nickname_id(&text);
        let _ = api::player::send_nn_message(cx, nag.player, nag.line);
        return;
    }
    let Some(line) = crate::content::npc_ships::pick_voice_line(cx, name, b"-", nag.voice) else {
        return;
    };
    nag.line = api::misc::nickname_id(&line);
    let Some(state) = cx.logic.content.state.players.get(&nag.player) else {
        return;
    };
    if !cx.logic.content.state.systems.contains_key(&nag.system) {
        return;
    }
    let ship = state.ship_id();
    if ship & ID_MASK == 0 {
        return;
    }
    let player_system = state.system.unwrap_or(0);
    let speaker_system = api::space_obj::system(cx, nag.speaker).unwrap_or(0);
    if speaker_system & ID_MASK != 0 && speaker_system == player_system {
        let _ = systems::send_comm(cx, nag.speaker, ship, nag.line, None, 0.5);
        return;
    }
    // A speaker without an `[NPC]` has voice 0, which has no lines, so a
    // comm here always has the record's costume.
    let speaker = CommSpeaker {
        voice: nag.voice,
        comm_type: nag.speaker_name,
        costume: nag.costume.clone().unwrap_or_default(),
    };
    let _ = systems::send_comm(cx, 0, ship, nag.line, Some(speaker), 0.5);
}

/// `FMissionNagDist::CountDown` (`MissionAct.cpp:1521-1528`): one nag
/// spent; the last fails the mission with the nag's mad lib.
fn count_down(cx: &mut Cx<'_>, id: ControllerId, dist: &mut NagDist) -> bool {
    dist.nags_left -= 1;
    if dist.nags_left != 0 {
        return true;
    }
    if let Some(s) = script_mut(cx, id) {
        engine::set_desired_state(s, engine::MISSION_STATE_FAIL, dist.fail_mad_lib);
    }
    false
}

/// `FMissionNagDist::Say(name)` (`MissionAct.cpp:1533-1548`): no line while
/// the speaker flies in the player's formation; the position form says
/// its own line. A nag without a speaker spends one with every line.
fn dist_say(cx: &mut Cx<'_>, id: ControllerId, nag: &mut Nag, dist: &mut NagDist, name: &[u8]) {
    let npc = if nag.speaker & ID_MASK != 0 {
        super::spawn::find_npc_by_id(cx, id, nag.speaker)
    } else {
        None
    };
    let npc_formation = npc
        .and_then(|k| cx.logic.content.state.npc_ships.get(&k))
        .map(|n| n.base.formation_id);
    let ship_formation = cx
        .logic
        .content
        .state
        .players
        .get(&nag.player)
        .and_then(|p| p.ship.as_ref())
        .map(|s| s.formation_id);
    let same_group = matches!((npc_formation, ship_formation),
        (Some(a), Some(b)) if a == b && a != u32::MAX);
    if !same_group {
        let line: &[u8] = if dist.position_mode {
            if dist.leaving_area {
                b"nag_leaving_mission_area"
            } else {
                b"nag_pos"
            }
        } else {
            name
        };
        say(cx, nag, line);
    }
    if nag.speaker & ID_MASK == 0 {
        count_down(cx, id, dist);
    }
}

/// The `StartNagTimer` block (`MissionAct.cpp:1554-1566`): threshold 900,
/// the next unique id, a 20 s timer on the mission.
fn start_timer(cx: &mut Cx<'_>, id: ControllerId, dist: &mut NagDist) {
    dist.threshold = 900.0;
    dist.timer = cx.logic.content.state.ids.take();
    let _ = api::controller::set_timer(cx, id, dist.timer, 0, 20.0);
}

/// A position of the player (`FContentPlayerState::GetPosition`).
fn position(cx: &mut Cx<'_>, player: u32) -> Vector {
    crate::content::random_mission::runtime::player_position(cx, player)
}

/// `FMissionNagGreet::HandleMessage` (`MissionAct.cpp:1468-1493`).
fn greet(cx: &mut Cx<'_>, nag: &mut Nag, message: &ControllerMessage<'_>) {
    let ControllerMessage::LaunchComplete { ship, .. } = *message else {
        return;
    };
    let Some(own) = cx
        .logic
        .content
        .state
        .players
        .get(&nag.player)
        .map(crate::content::players::PlayerState::ship_id)
    else {
        return;
    };
    if nag.speaker & ID_MASK == 0 || !cx.logic.content.state.systems.contains_key(&nag.system) {
        return;
    }
    if own != ship || own & ID_MASK == 0 {
        return;
    }
    let state = &cx.logic.content.state;
    let Some(speaker) = crate::content::ships::find_space_obj(state, nag.system, nag.speaker)
        .and_then(|k| state.npc_ships.get(&k))
    else {
        return;
    };
    if speaker.base.in_combat {
        say(cx, nag, b"nag_greet_combat");
    } else {
        say(cx, nag, b"nag_greet_normal");
    }
}

/// `FMissionNagDist::HandleMessage` (`MissionAct.cpp:1571-1748`).
#[allow(clippy::too_many_lines)]
fn dist(
    cx: &mut Cx<'_>,
    id: ControllerId,
    nag: &mut Nag,
    dist: &mut NagDist,
    message: &ControllerMessage<'_>,
) {
    use ControllerMessage as M;
    if !cx.logic.content.state.systems.contains_key(&nag.system) {
        return;
    }
    let Some(own) = cx
        .logic
        .content
        .state
        .players
        .get(&nag.player)
        .map(crate::content::players::PlayerState::ship_id)
    else {
        return;
    };
    if own & ID_MASK == 0 {
        return;
    }
    match *message {
        M::Heartbeat { .. } => {
            if dist.target & ID_MASK != 0
                && let Ok((p, _)) = api::space_obj::location(cx, dist.target)
            {
                dist.position = p;
            }
            if dist.armed {
                return;
            }
            let here = position(cx, nag.player);
            let dx = f64::from(dist.position.x) - f64::from(here.x);
            let dy = f64::from(dist.position.y) - f64::from(here.y);
            let dz = f64::from(dist.position.z) - f64::from(here.z);
            let dist_sq = dz * dz + dy * dy + dx * dx;
            let range = f64::from(dist.distance);
            if !(range * range < dist_sq) {
                return;
            }
            dist.last_position = position(cx, nag.player);
            dist.armed = true;
            start_timer(cx, id, dist);
        }
        M::CommComplete {
            speaker,
            line,
            result,
            ..
        } => {
            if nag.speaker != speaker && speaker & ID_MASK != 0 {
                return;
            }
            if nag.line != line || result != CommResult::Success {
                return;
            }
            count_down(cx, id, dist);
        }
        M::TimerExpired { param1, .. } => {
            if !dist.armed || param1 != dist.timer {
                return;
            }
            let p = dist.position;
            let last_x = (f64::from(dist.last_position.x) - f64::from(p.x)) as f32;
            let last_y = (f64::from(dist.last_position.y) - f64::from(p.y)) as f32;
            let last_z = (f64::from(dist.last_position.z) - f64::from(p.z)) as f32;
            let here = position(cx, nag.player);
            let ex = f64::from(here.x) - f64::from(p.x);
            let ey = f64::from(here.y) - f64::from(p.y);
            let ez = f64::from(here.z) - f64::from(p.z);
            let here_dist = (ez * ez + ey * ey + ex * ex).sqrt();
            let last_dist = (f64::from(last_z) * f64::from(last_z)
                + f64::from(last_y) * f64::from(last_y)
                + f64::from(last_x) * f64::from(last_x))
            .sqrt();
            if !(last_dist - here_dist < f64::from(dist.threshold))
                || !(here_dist > f64::from(dist.distance))
            {
                dist.timer = 0;
                dist.armed = false;
                dist.nags_left = 4;
                return;
            }
            dist.last_position = position(cx, nag.player);
            if dist.target & ID_MASK == 0 {
                dist_say(cx, id, nag, dist, b"nag_pos");
            } else if let Ok(kind) = api::space_obj::object_type(cx, dist.target) {
                if kind & 0x7f_0000 != 0 {
                    let state = &cx.logic.content.state;
                    let combat =
                        crate::content::ships::find_space_obj(state, nag.system, dist.target)
                            .and_then(|k| state.npc_ships.get(&k))
                            .map(|n| n.base.in_combat);
                    match combat {
                        Some(true) if dist.nag_in_combat => {
                            dist_say(cx, id, nag, dist, b"nag_dst_ship");
                        }
                        Some(false) if dist.nag_out_of_combat => {
                            dist_say(cx, id, nag, dist, b"nag_pos");
                        }
                        Some(_) => dist.armed = false,
                        None => {}
                    }
                } else if kind & 0x100 != 0 || kind & 0x20 != 0 {
                    dist_say(cx, id, nag, dist, b"nag_dst_base");
                } else if kind & 0x800 != 0 {
                    dist_say(cx, id, nag, dist, b"nag_dst_hole");
                } else if kind & 0xc40 != 0 {
                    dist_say(cx, id, nag, dist, b"nag_dst_gate");
                } else if kind & 0x80 != 0 {
                    dist_say(cx, id, nag, dist, b"nag_dst_tl");
                }
            }
            if !dist.armed {
                return;
            }
            dist.last_position = position(cx, nag.player);
            start_timer(cx, id, dist);
        }
        M::DockComplete { ship, dock, .. } => {
            let Ok(kind) = api::space_obj::object_type(cx, dist.target) else {
                return;
            };
            if kind & 0xc40 == 0 || dist.target != dock || own != ship {
                return;
            }
            nag.done = true;
        }
        M::TradelaneEnter { ship, entrance, .. } => {
            if dist.target != entrance || own != ship {
                return;
            }
            nag.done = true;
        }
        M::Msg0x27 {
            ship, base, target, ..
        } => {
            if ship != own || ship & ID_MASK == 0 {
                return;
            }
            let Ok(kind) = api::space_obj::object_type(cx, dist.target) else {
                return;
            };
            let dock_target = api::space_obj::docking_target(cx, dist.target).unwrap_or(0);
            if kind & 0x100 != 0 && base & ID_MASK != 0 && dock_target == base {
                nag.done = true;
                return;
            }
            if kind & 0x20 != 0 && dist.target == target {
                nag.done = true;
            }
        }
        _ => {}
    }
}

/// `DispatchNagMessage(Script, message)` (`MissionAct.cpp:1751-1800`): a
/// `SystemEnter` moves the entering player's nags to the new system; then,
/// unless the nags are clamped, each nag that hears the message handles
/// it in id order, and one that is done leaves the map (the C++ does not
/// delete it).
pub fn dispatch(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let Some(s) = script_mut(cx, id) else { return };
    if let ControllerMessage::SystemEnter { system, player, .. } = *message {
        for nag in s.nags.values_mut() {
            if nag.player == player {
                nag.system = system;
            }
        }
    }
    if s.nag_clamp {
        return;
    }
    let event = message.id();
    let keys: Vec<u32> = s
        .nags
        .iter()
        .filter(|(_, n)| n.listens(event))
        .map(|(k, _)| *k)
        .collect();
    for key in keys {
        let Some(mut nag) = script_mut(cx, id).and_then(|s| s.nags.remove(&key)) else {
            continue;
        };
        let mut kind = std::mem::replace(&mut nag.kind, NagKind::Greet);
        match &mut kind {
            NagKind::Greet => greet(cx, &mut nag, message),
            NagKind::Dist(d) => dist(cx, id, &mut nag, d, message),
        }
        nag.kind = kind;
        if !nag.done
            && let Some(s) = script_mut(cx, id)
        {
            s.nags.entry(key).or_insert(nag);
        }
    }
}
