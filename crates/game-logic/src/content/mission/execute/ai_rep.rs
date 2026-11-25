//! The AI and reputation actions (`Content/MissionAct.cpp`; work package
//! P3.5c3): `Act::Execute` of `Act_GiveObjList`, `Act_GiveMB`,
//! `Act_SetPriority`, `Act_PilotParams`, `Act_SetFlee`, `Act_PlayerForm`,
//! `Act_SetVibe`, `Act_SetVibeLbl`, `Act_SetVibeLblToShip`,
//! `Act_SetVibeShipToLbl`, `Act_SetVibeOfferBaseHack`, `Act_SetRep` and
//! `Act_RepChangeRequest`, with the script's `SetObjectVibe`
//! (`MissionAct.cpp:3945-4028`).
//!
//! The mission's `[ObjList]` lists are handed out as the shared counted
//! directives the spawns use ([`spawn::obj_list_refs`]); a ship runs them
//! through [`crate::content::mission::obj_list_execute`].

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_server::controller::ControllerId;

use super::super::action::Action;
use super::super::runtime::{
    ID_MASK, find_npc_by_name, label_object_id, player_state, script, script_mut,
};
use super::super::spawn;
use crate::api;
use crate::content::guides::{self, GuideDesc};
use crate::content::ships::ShipKey;
use crate::content::{dock_queues, formations, npc_ships};
use crate::events::NpcShipKey;
use crate::server::Cx;

/// `Player`, the label `Act_SetRep` compares exactly (`strcmp`).
const PLAYER: &[u8] = b"Player";

/// The NPC ship the mission knows by `label` (`FindNpcByName`).
fn npc_by_name(cx: &Cx<'_>, id: ControllerId, label: &[u8]) -> Option<NpcShipKey> {
    script(cx, id).and_then(|s| find_npc_by_name(cx, s, label))
}

/// The mission's object id of `label` (`GetLabelObjectId`), 0 without
/// the script.
fn label_object(cx: &Cx<'_>, id: ControllerId, label: &[u8]) -> u32 {
    script(cx, id).map_or(0, |s| label_object_id(cx, s, label))
}

/// The members of the label map (`+0xec`) under `label`, in the order
/// they joined it.
fn label_members(cx: &Cx<'_>, id: ControllerId, label: u32) -> Vec<u32> {
    script(cx, id)
        .and_then(|s| s.object.formation_members.get(&label).cloned())
        .unwrap_or_default()
}

/// The mission has a live player state (`FindContentPlayerState`).
fn has_player_state(cx: &Cx<'_>, id: ControllerId) -> bool {
    script(cx, id).is_some_and(|s| player_state(cx, s).is_some())
}

/// The tail of `SetObjectVibe` for one object (`RecordVibeSolar`,
/// `MissionAct.cpp:3952-3967`): a set id whose type (0 when `GetType`
/// fails) has a bit of `0xfdff` and that is no `[MsnSolar]` of the mission
/// adds its solar reputation (-1 when `GetSolarRep` fails) to the
/// mission's solar reputations, once.
fn record_vibe_solar(cx: &mut Cx<'_>, id: ControllerId, object: u32) {
    if object & ID_MASK == 0 {
        return;
    }
    let kind = api::space_obj::object_type(cx, object).unwrap_or(0);
    if kind & 0xfdff == 0 {
        return;
    }
    if script(cx, id).is_some_and(|s| s.object.solars.iter().any(|o| o.space_obj_id == object)) {
        return;
    }
    let rep = api::space_obj::solar_rep(cx, object).unwrap_or(-1);
    if let Some(s) = script_mut(cx, id)
        && !s.object.solar_reps.contains(&rep)
    {
        s.object.solar_reps.push(rep);
    }
}

/// `FMissionScript::SetObjectVibe(A, B, Lo, Hi)` (`MissionAct.cpp:4012-4028`):
/// A's attitude towards B ([`dock_queues::set_object_attitude`]); when A
/// is one of the mission's NPC ships that leads its formation or flies in
/// none, every member of its formation takes the same attitude towards B;
/// then the solar bookkeeping of A and of B. Returns the attitude.
pub fn set_object_vibe(cx: &mut Cx<'_>, id: ControllerId, a: u32, b: u32, lo: f32, hi: f32) -> f32 {
    let result = dock_queues::set_object_attitude(cx, a, b, lo, hi);
    if let Some(key) = spawn::find_npc_by_id(cx, id, a)
        && let Some(n) = npc_ships::get(cx, key)
        && (n.base.formation_slot == 0 || n.base.formation_id == u32::MAX)
    {
        let state = &cx.logic.content.state;
        let members: Vec<u32> = formations::collect_members(state, n.base.formation_id)
            .into_iter()
            .map(|m| crate::content::ships::id(state, m))
            .collect();
        for member in members {
            let _ = dock_queues::set_object_attitude(cx, member, b, result, result);
        }
    }
    record_vibe_solar(cx, id, a);
    record_vibe_solar(cx, id, b);
    result
}

/// `Act_GiveObjList` (`MissionAct.cpp:2841-2863`): with a live player, the
/// NPC ship of the label, else the player's ship for `Player`, takes the
/// `[ObjList]` (an NPC ship drops its guide first, `SetGuide(null, true,
/// false)`), at priority 0.
fn give_obj_list(cx: &mut Cx<'_>, id: ControllerId, npc: &[u8], list: u32) {
    if !has_player_state(cx, id) {
        return;
    }
    let ship = match npc_by_name(cx, id, npc) {
        Some(key) => ShipKey::Npc(key),
        None => {
            if !eq_ignore_case(PLAYER, c_str(npc)) {
                return;
            }
            let Some(player) = script(cx, id)
                .filter(|s| player_state(cx, s).is_some_and(|p| p.ship.is_some()))
                .map(|s| s.object.player)
            else {
                return;
            };
            ShipKey::Player(player)
        }
    };
    let Some(refs) = spawn::obj_list_refs(cx, id, list) else {
        return;
    };
    let own = crate::content::ships::id(&cx.logic.content.state, ship);
    if own != spawn::player_ship_id(cx, id)
        && let ShipKey::Npc(key) = ship
    {
        guides::set_guide(cx, key, None, true, false);
    }
    npc_ships::set_directives(cx, ship, &refs, 0);
}

/// `Act_GiveMB` (`MissionAct.cpp:2801-2819`): the NPC ship of the label
/// cancels its directives and takes the guide of the `[MetaBehavior]`'s
/// first object, started (`SetGuideFromDesc(First, true)`); an empty list
/// gives none.
fn give_mb(cx: &mut Cx<'_>, id: ControllerId, npc: &[u8], list: u32) {
    let Some(key) = npc_by_name(cx, id, npc) else {
        return;
    };
    let Some(items) = script(cx, id).and_then(|s| s.object.meta_behaviors.get(&list).cloned())
    else {
        return;
    };
    npc_ships::cancel_directives(cx, ShipKey::Npc(key));
    let Some(first) = items.into_iter().next() else {
        return;
    };
    guides::set_guide_from_desc(cx, key, &GuideDesc::from_meta(first), true);
}

/// `Act_SetFlee` (`MissionAct.cpp:2705-2725`).
fn set_flee(cx: &mut Cx<'_>, id: ControllerId, a: &super::super::action::SetFlee) {
    let Some(key) = npc_by_name(cx, id, &a.npc) else {
        return;
    };
    match a.mode {
        0 => npc_ships::set_flee_position(cx, key, a.position, a.distance),
        1 => {
            let target = label_object(cx, id, &a.target);
            if target & ID_MASK != 0 {
                npc_ships::set_flee_object(cx, key, target, a.distance, a.flag);
            }
        }
        2 => npc_ships::clear_flee(cx, key),
        _ => {}
    }
}

/// `Act_PlayerForm` (`MissionAct.cpp:3826-3835`): with a live player, the
/// player's ship and the label's NPC ship form the action's formation. The
/// C++ passes a null ship on as it is, which `FormFormation` dereferences;
/// the port leaves it out (D991).
fn player_form(cx: &mut Cx<'_>, id: ControllerId, a: &super::super::action::PlayerForm) {
    let Some(player) = script(cx, id)
        .filter(|s| player_state(cx, s).is_some())
        .map(|s| s.object.player)
    else {
        return;
    };
    let mut ships = Vec::new();
    if cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .is_some_and(|p| p.ship.is_some())
    {
        ships.push(ShipKey::Player(player));
    }
    if let Some(key) = npc_by_name(cx, id, &a.npc) {
        ships.push(ShipKey::Npc(key));
    }
    formations::form_formation(cx, &mut ships, a.formation, false);
}

/// `Act_SetVibeLbl` (`MissionAct.cpp:4088-4130`): for each NPC ship of the
/// mission labelled A, every NPC ship labelled B and then every
/// `[MsnSolar]` labelled B; then for each `[MsnSolar]` labelled A, every
/// NPC ship labelled B. The C++ tests only the inner NPC ships for null
/// (an entry SpaceSupport deleted, D979); the port skips every ship it no
/// longer has (D992).
fn set_vibe_lbl(cx: &mut Cx<'_>, id: ControllerId, a: &super::super::action::SetVibeLbl) {
    let Some(s) = script(cx, id) else {
        return;
    };
    let state = &cx.logic.content.state;
    let npcs: Vec<(u32, Vec<u32>)> = s
        .object
        .npc_objects
        .values()
        .filter_map(|k| state.npc_ships.get(k))
        .map(|n| (n.base.ship, n.field_0x17c.clone()))
        .collect();
    let solars: Vec<(u32, Vec<u32>)> = s
        .object
        .solars
        .iter()
        .map(|o| (o.space_obj_id, o.label_ids.clone()))
        .collect();
    let mut pairs = Vec::new();
    for (ship_a, labels_a) in &npcs {
        if !labels_a.contains(&a.label_a) {
            continue;
        }
        for (ship_b, labels_b) in &npcs {
            if labels_b.contains(&a.label_b) && ship_a & ID_MASK != 0 && ship_b & ID_MASK != 0 {
                pairs.push((*ship_a, *ship_b));
            }
        }
        for (solar, labels) in &solars {
            if labels.contains(&a.label_b) && ship_a & ID_MASK != 0 && solar & ID_MASK != 0 {
                pairs.push((*ship_a, *solar));
            }
        }
    }
    for (solar, labels) in &solars {
        if !labels.contains(&a.label_a) {
            continue;
        }
        for (ship_b, labels_b) in &npcs {
            if labels_b.contains(&a.label_b) && solar & ID_MASK != 0 && ship_b & ID_MASK != 0 {
                pairs.push((*solar, *ship_b));
            }
        }
    }
    for (x, y) in pairs {
        set_object_vibe(cx, id, x, y, a.lo, a.hi);
    }
}

/// `Act_SetVibeOfferBaseHack` (`MissionAct.cpp:371-387`): with the
/// player's state and ship, the base's solar reputation takes the attitude
/// 1 towards the player's.
fn set_vibe_offer_base_hack(cx: &mut Cx<'_>, id: ControllerId, base: u32) {
    let Some(player) = script(cx, id)
        .filter(|s| player_state(cx, s).is_some_and(|p| p.ship.is_some()))
        .map(|s| s.object.player)
    else {
        return;
    };
    let Ok(base_rep) = api::space_obj::solar_rep(cx, base) else {
        return;
    };
    let Ok(player_rep) = api::player::rep(cx, player) else {
        return;
    };
    let _ = api::reputation::set_attitude(cx, base_rep, player_rep, 1.0);
}

/// `Act_SetRep` (`MissionAct.cpp:3877-3899`): false for a label that finds
/// no object (but `Player`) or an unknown group; `Player` with a live
/// player is the player's reputation, any other label its object's, set
/// towards the group. The second label lookup of the C++'s object branch
/// (`FindNpcByName`, its result unused) is left out.
fn set_rep(cx: &mut Cx<'_>, id: ControllerId, a: &super::super::action::SetRep) -> bool {
    let object = label_object(cx, id, &a.label);
    let is_player = c_str(&a.label) == PLAYER;
    if object & ID_MASK == 0 && !is_player {
        return false;
    }
    if a.group_id & ID_MASK == 0 {
        return false;
    }
    let rep = if is_player && has_player_state(cx, id) {
        let player = script(cx, id).map_or(0, |s| s.object.player);
        api::player::rep(cx, player)
    } else {
        api::space_obj::rep(cx, object)
    };
    if let Ok(rep) = rep {
        let _ = api::reputation::set_reputation(cx, rep, a.group_id, a.value);
    }
    true
}

/// The 13 actions of P3.5c3; true (the C++'s result, unused) for any
/// other, which the caller does not pass.
pub fn execute(cx: &mut Cx<'_>, id: ControllerId, action: &mut Action) -> bool {
    match action {
        Action::GiveObjList(a) => {
            give_obj_list(cx, id, &a.npc, a.list_id);
            true
        }
        Action::GiveMb(a) => {
            give_mb(cx, id, &a.npc, a.list_id);
            true
        }
        // `Act_SetPriority` (`MissionAct.cpp:2638-2653`): every NPC ship
        // under the label takes the priority.
        Action::SetPriority(a) => {
            for member in label_members(cx, id, a.formation_id) {
                if let Some(key) = spawn::find_npc_by_id(cx, id, member) {
                    npc_ships::set_priority(cx, ShipKey::Npc(key), a.priority);
                }
            }
            true
        }
        // `Act_PilotParams` (`MissionAct.cpp:2551-2557`): the NPC ship's
        // personality submitted again in the action's flee mode.
        Action::PilotParams(a) => {
            if let Some(key) = npc_by_name(cx, id, &a.label) {
                spawn::submit_npc_personality(cx, key, a.flee_mode);
            }
            true
        }
        Action::SetFlee(a) => {
            set_flee(cx, id, a);
            true
        }
        Action::PlayerForm(a) => {
            player_form(cx, id, a);
            true
        }
        // `Act_SetVibe` (`MissionAct.cpp:4053-4060`).
        Action::SetVibe(a) => {
            let x = label_object(cx, id, &a.label_a);
            let y = label_object(cx, id, &a.label_b);
            if x & ID_MASK != 0 && y & ID_MASK != 0 {
                set_object_vibe(cx, id, x, y, a.lo, a.hi);
            }
            true
        }
        Action::SetVibeLbl(a) => {
            set_vibe_lbl(cx, id, a);
            true
        }
        // `Act_SetVibeLblToShip` (`MissionAct.cpp:4157-4169`): every member
        // under the label towards the ship.
        Action::SetVibeLblToShip(a) => {
            let ship = label_object(cx, id, &a.ship);
            for member in label_members(cx, id, a.formation_id) {
                if member & ID_MASK != 0 && ship & ID_MASK != 0 {
                    set_object_vibe(cx, id, member, ship, a.lo, a.hi);
                }
            }
            true
        }
        // `Act_SetVibeShipToLbl` (`MissionAct.cpp:4195-4207`): the ship
        // towards every member under the label.
        Action::SetVibeShipToLbl(a) => {
            let ship = label_object(cx, id, &a.ship);
            for member in label_members(cx, id, a.formation_id) {
                if ship & ID_MASK != 0 && member & ID_MASK != 0 {
                    set_object_vibe(cx, id, ship, member, a.lo, a.hi);
                }
            }
            true
        }
        Action::SetVibeOfferBaseHack(a) => {
            set_vibe_offer_base_hack(cx, id, a.base_id);
            true
        }
        Action::SetRep(a) => set_rep(cx, id, a),
        // `Act_RepChangeRequest` (`MissionAct.cpp:3626-3633`): the
        // reputation pop-up's state.
        Action::RepChangeRequest(a) => {
            if let Some(s) = script_mut(cx, id) {
                s.rep_pop_up_shown = a.shown;
                s.rep_pop_up_armed = a.armed;
                s.rep_pop_up_system = a.system_id;
                s.rep_pop_up_groups.clone_from(&a.groups);
            }
            true
        }
        _ => true,
    }
}
