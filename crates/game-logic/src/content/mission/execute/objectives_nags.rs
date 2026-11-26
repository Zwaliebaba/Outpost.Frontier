//! `Execute` of the objective, NN path and nag classes (work package
//! P3.5c5, `Content/MissionAct.cpp`): `Act_GiveNNObjs` (`:477-481`),
//! `Act_SetNNHidden` (`:983-991`), `Act_SetNNObj` (`:1019-1027`),
//! `Act_SetNNState` (`:1063-1067`), `Act_NNPath` (`:3780-3798`),
//! `Act_NagGreet` (`:1821-1838`), `Act_NagDistLeaving` (`:1897-1931`),
//! `Act_NagDistTowards` (`:1994-2026`) and `Act_NagOff` (`:2044-2053`),
//! over the objectives' run time ([`super::super::objectives`]), the NN
//! path ([`super::super::nn_path`]) and the nags ([`super::super::nags`]).

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_server::controller::ControllerId;

use super::super::action::Action;
use super::super::nags::{self, Nag, NagDist, NagKind};
use super::super::runtime::{ID_MASK, label_object_id, player_state, script, script_mut};
use super::super::{nn_path, objectives};
use crate::api;
use crate::server::Cx;

/// `_stricmp(text, "none") == 0`.
fn is_none(text: &[u8]) -> bool {
    eq_ignore_case(c_str(text), b"none")
}

/// `GetLabelObjectId(label)` of mission `id`.
fn label_object(cx: &Cx<'_>, id: ControllerId, label: &[u8]) -> u32 {
    script(cx, id).map_or(0, |s| label_object_id(cx, s, label))
}

/// `ReplaceNag(Mission, Id, Nag)` (`MissionAct.cpp:1350-1360`): an old
/// nag of the id goes, the new one takes its place.
fn replace_nag(cx: &mut Cx<'_>, id: ControllerId, nag_id: u32, nag: Nag) {
    if let Some(s) = script_mut(cx, id) {
        s.nags.insert(nag_id, nag);
    }
}

/// `Act::Execute` of the P3.5c5 action classes.
#[allow(clippy::too_many_lines)]
pub fn execute(cx: &mut Cx<'_>, id: ControllerId, action: &mut Action) -> bool {
    let has_state = script(cx, id).is_some_and(|s| player_state(cx, s).is_some());
    match action {
        Action::GiveNnObjs(_) => {
            objectives::renumber_all(cx, id);
            true
        }
        Action::SetNnHidden(a) => {
            if a.objective_id == create_id(b"all") {
                objectives::hide_all(cx, id);
                return true;
            }
            if let Some(s) = script_mut(cx, id) {
                nn_path::reset(&mut s.nn_path);
            }
            objectives::set_visible(cx, id, a.objective_id, a.visible)
        }
        Action::SetNnObj(a) => {
            if let Some(s) = script_mut(cx, id) {
                nn_path::reset(&mut s.nn_path);
            }
            objectives::hide_all(cx, id);
            objectives::set_state(cx, id, a.objective_id, a.state as u32);
            objectives::set_visible(cx, id, a.objective_id, true);
            objectives::renumber_all(cx, id);
            true
        }
        Action::SetNnState(a) => {
            objectives::set_state(cx, id, a.objective_id, a.state as u32);
            true
        }
        Action::NnPath(a) => {
            let Some(s) = script(cx, id) else {
                return false;
            };
            let Some(system) = player_state(cx, s).and_then(|p| p.system) else {
                return false;
            };
            if !cx.logic.content.state.systems.contains_key(&system) {
                return false;
            }
            let Some(s) = script_mut(cx, id) else {
                return false;
            };
            let path = &mut s.nn_path;
            nn_path::reset(path);
            path.position = a.position;
            path.travel_ids = a.travel_ids;
            path.target_ids = a.target_ids;
            path.system_id = if a.system_id & ID_MASK != 0 {
                a.system_id
            } else {
                system
            };
            if !c_str(&a.label).is_empty() {
                path.label.clone_from(&a.label);
            }
            path.state = 1;
            path.active = true;
            true
        }
        Action::NagGreet(a) => {
            if !has_state {
                return true;
            }
            let mut nag = Nag::new(NagKind::Greet);
            nag.speaker = label_object(cx, id, &a.speaker);
            nags::fill_speaker(cx, id, &mut nag, &a.speaker);
            // A unique insert: an existing nag of the id stays.
            if let Some(s) = script_mut(cx, id) {
                s.nags.entry(a.nag_id).or_insert(nag);
            }
            true
        }
        Action::NagDistLeaving(a) => {
            if !has_state {
                return true;
            }
            let mut nag = Nag::new(NagKind::Greet);
            nag.speaker = if is_none(&a.speaker) {
                0
            } else {
                label_object(cx, id, &a.speaker)
            };
            nags::fill_speaker(cx, id, &mut nag, &a.speaker);
            let mut dist = NagDist::default();
            let target = c_str(&a.target);
            if !target.is_empty() && !eq_ignore_case(target, b"position") {
                dist.target = label_object(cx, id, &a.target);
            } else {
                nag.speaker = 0;
                dist.leaving_area = true;
            }
            dist.distance = a.distance;
            dist.fail_mad_lib = a.fail_mad_lib_id;
            dist.nag_in_combat = a.nag_in_combat;
            dist.nag_out_of_combat = a.nag_out_of_combat;
            dist.position_mode = a.has_position;
            if a.has_position {
                dist.position = a.position;
            }
            nag.kind = NagKind::Dist(dist);
            replace_nag(cx, id, a.nag_id, nag);
            true
        }
        Action::NagDistTowards(a) => {
            if !has_state {
                return true;
            }
            let mut nag = Nag::new(NagKind::Greet);
            nag.speaker = if is_none(&a.speaker) {
                0
            } else {
                label_object(cx, id, &a.speaker)
            };
            nags::fill_speaker(cx, id, &mut nag, &a.speaker);
            let mut dist = NagDist {
                nag_in_combat: a.nag_in_combat,
                nag_out_of_combat: a.nag_out_of_combat,
                position_mode: true,
                ..NagDist::default()
            };
            if c_str(&a.target).is_empty() {
                dist.position = a.position;
                dist.target = 0;
            } else {
                dist.target = label_object(cx, id, &a.target);
                if let Ok((p, _)) = api::space_obj::location(cx, dist.target) {
                    dist.position = p;
                }
            }
            dist.distance = a.distance;
            dist.fail_mad_lib = a.fail_mad_lib_id;
            nag.kind = NagKind::Dist(dist);
            replace_nag(cx, id, a.nag_id, nag);
            true
        }
        Action::NagOff(a) => {
            if let Some(s) = script_mut(cx, id) {
                s.nags.remove(&a.nag_id);
            }
            true
        }
        _ => true,
    }
}
