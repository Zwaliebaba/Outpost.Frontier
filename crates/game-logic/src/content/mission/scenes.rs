//! The script mission's cut scenes, ambient scenes, static cameras and
//! costume hints (`Content/MissionScript.cpp`, TU21; work package P3.5c4).
//!
//! - **Cut scenes** (`List_0x2f4`, [`super::MissionScript::rtc_scripts`]):
//!   `Act_AddRTC` appends one and starts it ([`start_rtc_script`]): on the
//!   player's base, the RTC script of its file ([`crate::content::rtc_scripts`])
//!   becomes a bar scene of the base ([`bar::create_bar_scene`]) with a new
//!   sequence id, which `Cnd_RTCDone` matches. The base, location and
//!   character cases of the handler start, finish and release them
//!   ([`on_base_enter`], [`on_base_exit`], [`on_character_select`],
//!   [`on_location_exit`], [`on_mission_changed`]).
//! - **Ambient scenes** (`AmbientScenes`): made on the player's base when
//!   it is entered (`AmbientScriptCreate`), destroyed when it is left.
//! - **Static cameras and scene scripts**: [`enter_static_scene`] (the tail
//!   of `Act_CallSceneScript` and `Act_StaticCam`) and [`revert_static_cam`].
//! - **Costume hints**: the dialog speakers' costumes, sent to the player on
//!   `SpaceEnter` ([`send_costume_hints`]).

use game_sim::data::costume::Costume;
use game_sim::protocol::base_props::AmbientScriptProperties;
use neuron_core::hash::c_str;
use neuron_server::controller::ControllerId;

use super::obj_list::DirectiveKind;
use super::runtime::{ID_MASK, player_state, script, script_mut};
use super::save::{AmbientScene, RtcScript};
use crate::api;
use crate::content::ships::ShipKey;
use crate::content::{bar, formations, npc_ships, rtc_scripts};
use crate::server::Cx;

/// The mission's player and the player's base record (`GetBaseRecord`),
/// when the player has a state; the base `None` without a record.
fn player_base(cx: &Cx<'_>, id: ControllerId) -> Option<(u32, Option<u32>)> {
    let s = script(cx, id)?;
    let state = player_state(cx, s)?;
    let base = state
        .base
        .filter(|b| cx.logic.content.state.bases.contains_key(b));
    Some((s.object.player, base))
}

/// `GetFirstRtcRole(Script)` (`MissionScript.cpp:571-578`): the first
/// role's name; a script without roles passes null, which `FindCharacter`
/// dereferences (D1040): none is found.
fn first_role(script: &bar::CharacterEncounter) -> Option<&[u8]> {
    script.roles.first().map(|r| r.role.as_slice())
}

/// `FBaseRoomState::FindCharacter(GetFirstRtcRole(Script), PlayerId)` in
/// the room state of `location` of `base`: the character's key and id.
fn find_scene_character(
    cx: &Cx<'_>,
    base: u32,
    location: u32,
    script: &bar::CharacterEncounter,
    player: u32,
) -> Option<(u32, u32)> {
    let role = first_role(script)?;
    let room = cx
        .logic
        .content
        .state
        .bases
        .get(&base)?
        .room_states
        .get(&location)?;
    room.find_character(role, player)
        .map(|c| (c.key, c.character_id))
}

/// `FMissionScript::StartRtcScript(Rtc)` (`MissionScript.cpp:748-776`):
/// with the player's state and base record, the RTC script of the record's
/// file, when it opens and is a script of that base, becomes a bar scene:
/// a script with a mission text keeps its loaded text when the player has
/// no live mission, takes a new sequence id (`AllocateFormationId`), and
/// the record keeps the scene's handle and the script. Otherwise the record
/// is left as it was.
pub fn start_rtc_script(cx: &mut Cx<'_>, id: ControllerId, rtc: &mut RtcScript) {
    let Some((player, Some(base))) = player_base(cx, id) else {
        return;
    };
    let Some(mut loaded) = rtc_scripts::load(cx, &rtc.path) else {
        return;
    };
    if loaded.base_id != base {
        return;
    }
    let live = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(0, |p| p.live_mission);
    if loaded.mission_text_id != 0 && live & ID_MASK == 0 {
        loaded.mission_text_id = loaded.loaded_mission_text_id;
    }
    loaded.sequence_id = formations::allocate_id(&mut cx.logic.content.state);
    rtc.handle = bar::create_bar_scene(cx, base, &loaded, player, id.0);
    rtc.script = Some(Box::new(loaded));
}

/// Runs `f` on the mission's cut scene at `at`, taken out of the list and
/// written back after it (nothing `f` calls reaches the list).
fn with_rtc(
    cx: &mut Cx<'_>,
    id: ControllerId,
    at: usize,
    f: impl FnOnce(&mut Cx<'_>, &mut RtcScript),
) {
    let Some(mut rtc) = script(cx, id).and_then(|s| s.rtc_scripts.get(at).cloned()) else {
        return;
    };
    f(cx, &mut rtc);
    if let Some(slot) = script_mut(cx, id).and_then(|s| s.rtc_scripts.get_mut(at)) {
        *slot = rtc;
    }
}

/// `RemoveMissionRtc(Script, &Path)` (`MissionScript.cpp:582-611`,
/// `Act_RemoveRTC`): every cut scene of the path (`strcmp`) is finished,
/// while the player has a state; a started one's first role's character in
/// the room of the mission's location on the player's base first loses its
/// behaviour (`SetBehavior(-1)`).
pub fn remove_mission_rtc(cx: &mut Cx<'_>, id: ControllerId, path: &[u8]) {
    let count = script(cx, id).map_or(0, |s| s.rtc_scripts.len());
    for at in 0..count {
        with_rtc(cx, id, at, |cx, rtc| {
            if c_str(&rtc.path) != c_str(path) {
                return;
            }
            let Some((player, base)) = player_base(cx, id) else {
                return;
            };
            let location = script(cx, id).map_or(0, |s| s.current_location);
            if let (Some(base), Some(loaded)) = (base, rtc.script.as_deref())
                && let Some((key, _)) = find_scene_character(cx, base, location, loaded, player)
            {
                bar::set_behavior(cx, base, key, u32::MAX);
            }
            rtc.finished = true;
        });
    }
}

/// `FMissionScript::ReleaseRtcScripts` (`MissionScript.cpp:723-745`, the
/// destructor's and `EndMission`'s): every started cut scene's bar scene
/// is removed from the player's base (with a handle and a base record),
/// the script dropped; then the list is emptied.
pub fn release_rtc_scripts(cx: &mut Cx<'_>, id: ControllerId) {
    let base = player_base(cx, id).and_then(|(_, b)| b);
    let list = script_mut(cx, id).map(|s| std::mem::take(&mut s.rtc_scripts));
    for rtc in list.into_iter().flatten() {
        if rtc.script.is_none() {
            continue;
        }
        if let Some(base) = base
            && rtc.handle != -1
        {
            bar::remove_bar_scene(cx, base, rtc.handle, None);
        }
    }
}

/// The restore's release of a started cut scene
/// ([`super::save::RestoreStep::ReleaseRtcScript`]): its bar scene removed
/// from the player's base when it has a handle.
pub fn release_restored_rtc(cx: &mut Cx<'_>, id: ControllerId, handle: i32) {
    if let Some((_, Some(base))) = player_base(cx, id)
        && handle != -1
    {
        bar::remove_bar_scene(cx, base, handle, None);
    }
}

/// `pub::GF::AmbientScriptCreate({Room, 1, 0, Path, PlayerId})`: the
/// scene's handle (`Act_AddAmbient`, `OnBaseEnter`).
pub fn create_ambient(cx: &mut Cx<'_>, player: u32, path: &[u8], room: u32) -> i32 {
    let props = AmbientScriptProperties {
        key: 0,
        room_id: room,
        script_id: 1,
        flag: false,
        name: Some(c_str(path).to_vec()),
    };
    api::gf::ambient_script_create(cx, &props, player).cast_signed()
}

/// Every ambient scene with a handle destroyed (`AmbientScriptDestroy`),
/// its handle -1 (`OnBaseExit`, the destructor).
pub fn destroy_ambient_handles(cx: &mut Cx<'_>, id: ControllerId) {
    let count = script(cx, id).map_or(0, |s| s.ambient_scenes.len());
    for at in 0..count {
        let Some(handle) = script(cx, id)
            .and_then(|s| s.ambient_scenes.get(at))
            .map(|a| a.handle)
            .filter(|&h| h != -1)
        else {
            continue;
        };
        let mut handle = handle.cast_unsigned();
        api::gf::ambient_script_destroy(cx, &mut handle);
        if let Some(scene) = script_mut(cx, id).and_then(|s| s.ambient_scenes.get_mut(at)) {
            scene.handle = -1;
        }
    }
}

/// `Act_AddAmbient::Execute` (`MissionAct.cpp:2420-2436`): the scene made
/// for the player, then appended with its handle.
pub fn add_ambient(cx: &mut Cx<'_>, id: ControllerId, path: &[u8], room: u32, base: u32) -> i32 {
    let player = script(cx, id).map_or(0, |s| s.object.player);
    let handle = create_ambient(cx, player, path, room);
    if let Some(s) = script_mut(cx, id) {
        s.ambient_scenes.push(AmbientScene {
            path: path.to_vec(),
            handle,
            room_id: room,
            base_id: base,
        });
    }
    handle
}

/// `Act_RemoveAmbient::Execute` (`MissionAct.cpp:2455-2470`): the first
/// scene of the path (`_stricmp`) destroyed (its handle as it is) and
/// unlinked.
pub fn remove_ambient(cx: &mut Cx<'_>, id: ControllerId, path: &[u8]) {
    let Some(at) = script(cx, id).and_then(|s| {
        s.ambient_scenes
            .iter()
            .position(|a| neuron_core::hash::eq_ignore_case(c_str(&a.path), c_str(path)))
    }) else {
        return;
    };
    let Some(scene) = script(cx, id).map(|s| s.ambient_scenes[at].clone()) else {
        return;
    };
    let mut handle = scene.handle.cast_unsigned();
    api::gf::ambient_script_destroy(cx, &mut handle);
    if let Some(s) = script_mut(cx, id) {
        s.ambient_scenes.remove(at);
    }
}

/// The tail of `Act_CallSceneScript` and `Act_StaticCam`
/// (`MissionAct.cpp:2175-2187`, `:3689-3700`), with the player's state: the
/// static camera flag, the player's ship (with an id) invincible
/// (`SetInvincible(ship, true, false, 0)`), the nag clamp, every mission
/// NPC ship muted (`SetField1b1(true)`) and the GCS clamp on.
pub fn enter_static_scene(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let Some(state) = player_state(cx, s) else {
        return;
    };
    let ship = state.ship.as_ref().map_or(0, |p| p.ship);
    let npcs: Vec<_> = s.object.npc_objects.values().copied().collect();
    if let Some(s) = script_mut(cx, id) {
        s.field_0x2c8 = true;
    }
    if ship & ID_MASK != 0 {
        let _ = api::space_obj::set_invincible(cx, ship, true, false, 0.0);
    }
    if let Some(s) = script_mut(cx, id) {
        s.nag_clamp = true;
    }
    for npc in npcs {
        npc_ships::set_muted(cx, npc, true);
    }
    cx.logic.content.state.clamps.gcs_clamp_disabled = 0;
}

/// `FMissionScript::RevertStaticCam` (`MissionScript.cpp:778-815`), with
/// the player's state: the static camera flag off, the player's ship
/// vulnerable again (`SetInvincible(id, false, true, 0)`, the id not
/// masked), the nag clamp off, every mission NPC ship unmuted; a player
/// with a ship object gives it a list of one avoidance directive (`true`)
/// at priority 0; the GCS clamp off.
pub fn revert_static_cam(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let Some(state) = player_state(cx, s) else {
        return;
    };
    let player = s.object.player;
    let ship = state.ship.as_ref().map(|p| p.ship);
    let npcs: Vec<_> = s.object.npc_objects.values().copied().collect();
    if let Some(s) = script_mut(cx, id) {
        s.field_0x2c8 = false;
    }
    let _ = api::space_obj::set_invincible(cx, ship.unwrap_or(0), false, true, 0.0);
    if let Some(s) = script_mut(cx, id) {
        s.nag_clamp = false;
    }
    for npc in npcs {
        npc_ships::set_muted(cx, npc, false);
    }
    if ship.is_some() {
        let mut kind = DirectiveKind::create(0).unwrap_or(DirectiveKind::Avoidance(
            super::obj_list::Avoidance::default(),
        ));
        if let DirectiveKind::Avoidance(a) = &mut kind {
            // `SetAvoidance(true)`.
            a.op.flag_id = 0;
            a.op.value = 1;
        }
        let arena = &mut cx.logic.content.state.directives;
        let directive = arena.create(kind);
        npc_ships::set_directives(cx, ShipKey::Player(player), &[directive], 0);
        cx.logic.content.state.directives.release(directive);
    }
    cx.logic.content.state.clamps.gcs_clamp_disabled = 1;
}

/// The bytes `CostumeHint::Serialize` gives (`common_baseproperties.cpp:
/// 707-713`): the costume's thirteen dwords, the four parts, the eight
/// accessory slots (those past the count zero, as the hint's constructor
/// leaves them) and the count.
fn costume_hint_bytes(costume: &Costume) -> Vec<u8> {
    let mut words = [0u32; 13];
    words[..4].copy_from_slice(&costume.part_ids);
    let count = costume.accessory_ids.len().min(8);
    words[4..4 + count].copy_from_slice(&costume.accessory_ids[..count]);
    words[12] = u32::try_from(costume.accessory_ids.len()).unwrap_or(u32::MAX);
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// The costume hints of `OnSpaceEnter` (`MissionScript.cpp:1527-1539`): a
/// `CostumeHint` (kind 1) per dialog costume, in order, to the player
/// (`pub::Player::LoadHint`).
pub fn send_costume_hints(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let player = s.object.player;
    let hints: Vec<Vec<u8>> = s.dialog_costumes.iter().map(costume_hint_bytes).collect();
    for hint in hints {
        api::player::load_hint(cx, player, 1, &hint);
    }
}

/// The cut scenes and ambient scenes of `OnBaseEnter`
/// (`MissionScript.cpp:1320-1340`): every cut scene without a bar scene
/// is started, then every ambient scene of the entered base made.
pub fn on_base_enter(cx: &mut Cx<'_>, id: ControllerId, base: u32) {
    let count = script(cx, id).map_or(0, |s| s.rtc_scripts.len());
    for at in 0..count {
        with_rtc(cx, id, at, |cx, rtc| {
            if rtc.handle == -1 {
                start_rtc_script(cx, id, rtc);
            }
        });
    }
    let Some(player) = script(cx, id).map(|s| s.object.player) else {
        return;
    };
    let count = script(cx, id).map_or(0, |s| s.ambient_scenes.len());
    for at in 0..count {
        let Some(scene) = script(cx, id).and_then(|s| s.ambient_scenes.get(at).cloned()) else {
            continue;
        };
        if scene.base_id != base {
            continue;
        }
        let handle = create_ambient(cx, player, &scene.path, scene.room_id);
        if let Some(slot) = script_mut(cx, id).and_then(|s| s.ambient_scenes.get_mut(at)) {
            slot.handle = handle;
        }
    }
}

/// The cut scenes and ambient scenes of `OnBaseExit`
/// (`MissionScript.cpp:1358-1394`): with the left base's record, every
/// started cut scene's bar scene is removed (the handle not tested) and its
/// script dropped, and a finished one unlinked; then every ambient scene's
/// handle is destroyed.
pub fn on_base_exit(cx: &mut Cx<'_>, id: ControllerId, base: u32) {
    if cx.logic.content.state.bases.contains_key(&base) {
        let mut at = 0;
        while let Some(mut rtc) = script(cx, id).and_then(|s| s.rtc_scripts.get(at).cloned()) {
            if rtc.script.is_some() {
                bar::remove_bar_scene(cx, base, rtc.handle, None);
                rtc.handle = -1;
                rtc.script = None;
            }
            let Some(s) = script_mut(cx, id) else { return };
            if rtc.finished {
                s.rtc_scripts.remove(at);
            } else {
                s.rtc_scripts[at] = rtc;
                at += 1;
            }
        }
    }
    destroy_ambient_handles(cx, id);
}

/// The cut scenes of `OnCharacterSelect` (`MissionScript.cpp:1397-1427`):
/// an unfinished, not repeatable, started cut scene whose first role's
/// character (in the room of the mission's location on the player's base)
/// is the selected one: the character loses its behaviour and the scene is
/// finished.
pub fn on_character_select(cx: &mut Cx<'_>, id: ControllerId, character: u32) {
    let count = script(cx, id).map_or(0, |s| s.rtc_scripts.len());
    for at in 0..count {
        with_rtc(cx, id, at, |cx, rtc| {
            if rtc.finished || rtc.field_0x95 {
                return;
            }
            let Some((player, base)) = player_base(cx, id) else {
                return;
            };
            let (Some(base), Some(loaded)) = (base, rtc.script.as_deref()) else {
                return;
            };
            let location = script(cx, id).map_or(0, |s| s.current_location);
            let Some((key, found)) = find_scene_character(cx, base, location, loaded, player)
            else {
                return;
            };
            if found != character {
                return;
            }
            bar::set_behavior(cx, base, key, u32::MAX);
            rtc.finished = true;
        });
    }
}

/// The cut scenes of `OnLocationExit` (`MissionScript.cpp:1430-1475`), on
/// the player's base: the roles of the unfinished started cut scenes of
/// the left location are kept, and every finished started one is released
/// (`RemoveBarScene` with those roles) and unlinked.
pub fn on_location_exit(cx: &mut Cx<'_>, id: ControllerId, location: u32) {
    let Some((_, Some(base))) = player_base(cx, id) else {
        return;
    };
    let keep: Vec<Vec<u8>> = script(cx, id)
        .map(|s| {
            s.rtc_scripts
                .iter()
                .filter(|r| !r.finished)
                .filter_map(|r| r.script.as_deref())
                .filter(|l| l.location == location)
                .flat_map(|l| l.roles.iter().map(|r| r.role.clone()))
                .collect()
        })
        .unwrap_or_default();
    let mut at = 0;
    while let Some(rtc) = script(cx, id).and_then(|s| s.rtc_scripts.get(at).cloned()) {
        if !rtc.finished || rtc.script.is_none() {
            at += 1;
            continue;
        }
        bar::remove_bar_scene(cx, base, rtc.handle, Some(&keep));
        if let Some(s) = script_mut(cx, id) {
            s.rtc_scripts.remove(at);
        }
    }
}

/// The cut scenes of `OnMessage100b` (`MissionScript.cpp:1671-1711`,
/// `PlayerMissionChanged` to another mission), with the player's state and
/// ship: the first started, unfinished cut scene with a mission text, on
/// the player's base, is released (its bar scene removed, the record
/// unlinked) and started again as a new record with its path and
/// repeatable flag, appended.
pub fn on_mission_changed(cx: &mut Cx<'_>, id: ControllerId, mission: u32) {
    let Some(s) = script(cx, id) else { return };
    let Some(state) = player_state(cx, s) else {
        return;
    };
    if state.ship.is_none() || mission == id.0 {
        return;
    }
    let mut restart = RtcScript {
        handle: -1,
        ..RtcScript::default()
    };
    let count = s.rtc_scripts.len();
    for at in 0..count {
        let Some(rtc) = script(cx, id).and_then(|s| s.rtc_scripts.get(at).cloned()) else {
            break;
        };
        let Some(loaded) = rtc.script.as_deref() else {
            continue;
        };
        if loaded.mission_text_id == 0 || rtc.finished {
            continue;
        }
        let Some((_, Some(base))) = player_base(cx, id) else {
            continue;
        };
        restart.path = super::tstring(&rtc.path, 0x80);
        restart.field_0x95 = rtc.field_0x95;
        bar::remove_bar_scene(cx, base, rtc.handle, None);
        if let Some(s) = script_mut(cx, id) {
            s.rtc_scripts.remove(at);
        }
        break;
    }
    if !restart.path.is_empty() {
        start_rtc_script(cx, id, &mut restart);
        if let Some(s) = script_mut(cx, id) {
            s.rtc_scripts.push(restart);
        }
    }
}

/// The cut scenes and ambient scenes of `FMissionScript::~FMissionScript`
/// (`MissionScript.cpp:683-720`): the cut scenes released, then every
/// ambient scene with a handle destroyed and the list emptied.
pub fn destruct(cx: &mut Cx<'_>, id: ControllerId) {
    release_rtc_scripts(cx, id);
    destroy_ambient_handles(cx, id);
    if let Some(s) = script_mut(cx, id) {
        s.ambient_scenes.clear();
    }
}
