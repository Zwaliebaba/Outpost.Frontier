//! The trigger engine and the script mission's handler
//! (`Content/MissionScript.cpp`, TU21; work package P3.5c1, decision
//! record 0008 point 6).
//!
//! - [`activate_trigger`] (`ActivateMissionTrigger`, `:373-423`): a trigger
//!   not yet listed becomes active, its condition's `OnActivate` runs, and
//!   the activation probe `Matches(-1, null)` fires it at once when it
//!   holds; it joins the active list unless it completed.
//! - [`deactivate_trigger`] (`:426-452`): the first listed entry of the id
//!   that is not complete goes inactive and leaves the list; its pending
//!   activations go too.
//! - [`dispatch`] (`DispatchMissionTriggerEvent`, `:455-514`): the active
//!   list in order. A listening, active trigger whose condition matches
//!   completes, runs its actions in line order, is armed again when
//!   repeatable, and leaves the list when it stays complete. Then the
//!   pending activations are drained in key order.
//! - [`tick_timers`] (`TickMissionTimers`, `:517-537`): the delayed actions.
//! - [`handle_message`], `FMissionObject::HandleMessage`
//!   (`MissionObject.cpp:129-142`) over `FMissionScript::HandleMissionMessage`
//!   (`MissionScript.cpp:1046-1314`), its tail and `ApplyDesiredMissionState`.
//!
//! **Order within a frame.** A heartbeat adds the frame time to the
//! mission's clock, runs the timers, dispatches `Heartbeat` with the frame
//! time, then `TetherObject` with no parameters (`:1167-1178`); every other
//! case dispatches once at most, the activations it queued are drained at
//! the end of each dispatch, and the desired mission state is applied after
//! the tail.
//!
//! **The walk** is record 0006 point 7's cursor walk over trigger ids: each
//! walk keeps its place in a stack the deactivation adjusts, so a nested
//! dispatch or an action's `Act_DeactTrig` leaves every walk on the entry
//! `std::list` would be on. A condition is taken out of its trigger for
//! `OnActivate` and `Matches` (which cannot dispatch, record 0008 point 1);
//! an action is cloned out before it runs and written back after it, and
//! the next one is looked up again. When the trigger is gone after an
//! action (a nested `PlayerLogout` tore the map down) the pass stops (D972).
//!
//! The mission object's body (`FMissionObject::HandleMissionMessage`) came
//! with P3.5c2: the NPC registration of `NpcShipCreated`, the jumpers, the
//! respawns, the snapshot and the vibes, `ObjectDestroyed` and the logouts
//! ([`super::spawn`]). P3.5c4 brought the cut scenes, ambient scenes and
//! costume hints of the base, location, character, space and
//! `PlayerMissionChanged` cases ([`super::scenes`], over P3.6e's bar
//! scenes). What waits for later increments (D973): the random encounters'
//! messages (P3.6d); the objectives, the objective timer, the group
//! members, the NN path and the nags of the tail (P3.5c5).

use game_sim::protocol::fmtstr::FmtStr;
use neuron_server::controller::ControllerId;

use super::condition::event::{HEARTBEAT, NPC_SYSTEM_ENTER, TETHER_OBJECT};
use super::execute;
use super::runtime::{self, ID_MASK, Parms, label_object_id, player_state, script, script_mut};
use super::trace::TraceRecord;
use super::trigger::{STATE_ACTIVE, STATE_COMPLETE, STATE_INACTIVE};
use super::{MissionScript, save, scenes, spawn};
use crate::api;
use crate::content::NoDispatch;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `G_MissionStateFail`.
pub const MISSION_STATE_FAIL: i32 = 0;
/// `G_MissionStateSucceed`.
pub const MISSION_STATE_SUCCEED: i32 = 1;

/// The event id of the activation probe (`(EMessage)-1`).
pub const PROBE: u32 = u32::MAX;

/// One delayed action (`FMissionTimer`, `MissionScript.h:62-66`): the
/// seconds left and the action, by its trigger and its place there.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MissionTimer {
    /// `Remaining`.
    pub remaining: f32,
    /// The action's trigger.
    pub trigger: u32,
    /// The action's place in the trigger's list.
    pub index: usize,
}

/// Appends a trace record when the mission's trace is on.
fn trace(cx: &mut Cx<'_>, id: ControllerId, record: impl FnOnce(u32) -> TraceRecord) {
    if let Some(s) = script_mut(cx, id)
        && let Some(t) = s.trace.as_mut()
    {
        t.records.push(record(t.tick));
    }
}

/// Removes the active entry at `at`, keeping every walk on its entry.
fn remove_active(s: &mut MissionScript, at: usize) {
    s.active_triggers.remove(at);
    for cursor in &mut s.walks {
        if *cursor > at {
            *cursor -= 1;
        }
    }
}

/// `OnActivate` of trigger `trigger`'s condition, taken out of its slot.
fn condition_on_activate(cx: &mut Cx<'_>, id: ControllerId, trigger: u32) {
    let Some(mut condition) = script_mut(cx, id)
        .and_then(|s| s.triggers.get_mut(&trigger))
        .and_then(|t| t.condition.take())
    else {
        return;
    };
    let take = NoDispatch::begin(&cx.logic.content);
    runtime::on_activate(cx, id, &mut condition);
    take.end(&cx.logic.content);
    if let Some(t) = script_mut(cx, id).and_then(|s| s.triggers.get_mut(&trigger)) {
        t.condition = Some(condition);
    }
}

/// `Matches(event, parms)` of trigger `trigger`'s condition, taken out of
/// its slot. A trigger without a condition does not match: the C++ calls
/// through the null pointer (D974).
fn condition_matches(
    cx: &mut Cx<'_>,
    id: ControllerId,
    trigger: u32,
    event: u32,
    parms: &Parms<'_, '_>,
) -> bool {
    let Some(mut condition) = script_mut(cx, id)
        .and_then(|s| s.triggers.get_mut(&trigger))
        .and_then(|t| t.condition.take())
    else {
        return false;
    };
    let take = NoDispatch::begin(&cx.logic.content);
    let matched = runtime::matches(cx, id, &mut condition, event, parms);
    take.end(&cx.logic.content);
    if let Some(t) = script_mut(cx, id).and_then(|s| s.triggers.get_mut(&trigger)) {
        t.condition = Some(condition);
    }
    matched
}

/// The state of trigger `trigger`, `None` when it is gone.
fn state_of(cx: &Cx<'_>, id: ControllerId, trigger: u32) -> Option<i32> {
    script(cx, id)?.triggers.get(&trigger).map(|t| t.state)
}

fn set_state(cx: &mut Cx<'_>, id: ControllerId, trigger: u32, state: i32) {
    if let Some(t) = script_mut(cx, id).and_then(|s| s.triggers.get_mut(&trigger)) {
        t.state = state;
    }
}

/// Runs the action at `index` of trigger `trigger` on a clone, which is
/// written back afterwards. False when the action is not there.
fn run_action(
    cx: &mut Cx<'_>,
    id: ControllerId,
    trigger: u32,
    index: usize,
    record: impl FnOnce(u32) -> TraceRecord,
) -> bool {
    let Some(mut action) = script(cx, id)
        .and_then(|s| s.triggers.get(&trigger))
        .and_then(|t| t.actions.get(index))
        .cloned()
    else {
        return false;
    };
    trace(cx, id, record);
    execute::execute(cx, id, trigger, index, &mut action);
    if let Some(slot) = script_mut(cx, id)
        .and_then(|s| s.triggers.get_mut(&trigger))
        .and_then(|t| t.actions.get_mut(index))
    {
        *slot = action;
    }
    true
}

/// A completed trigger's actions in line order; false when the trigger was
/// torn down during one of them (D972).
fn run_actions(cx: &mut Cx<'_>, id: ControllerId, trigger: u32) -> bool {
    let mut index = 0;
    loop {
        let Some(count) = script(cx, id)
            .and_then(|s| s.triggers.get(&trigger))
            .map(|t| t.actions.len())
        else {
            return false;
        };
        if index >= count {
            return true;
        }
        let at = u32::try_from(index).unwrap_or(u32::MAX);
        run_action(cx, id, trigger, index, |tick| TraceRecord::Action {
            tick,
            trigger,
            index: at,
        });
        index += 1;
    }
}

/// `RearmTrigger` (`MissionScript.cpp:351-357`), when the trigger is
/// repeatable: active again, its condition's `OnActivate`.
fn rearm_if_repeatable(cx: &mut Cx<'_>, id: ControllerId, trigger: u32) {
    let repeatable = script(cx, id)
        .and_then(|s| s.triggers.get(&trigger))
        .is_some_and(|t| t.repeatable);
    if !repeatable {
        return;
    }
    set_state(cx, id, trigger, STATE_ACTIVE);
    trace(cx, id, |tick| TraceRecord::Rearm { tick, trigger });
    condition_on_activate(cx, id, trigger);
}

/// `ActivateMissionTrigger` (`MissionScript.cpp:373-423`).
pub fn activate_trigger(cx: &mut Cx<'_>, id: ControllerId, trigger: u32) {
    let Some(s) = script_mut(cx, id) else { return };
    if !s.triggers.contains_key(&trigger) || s.active_triggers.contains(&trigger) {
        return;
    }
    set_state(cx, id, trigger, STATE_ACTIVE);
    trace(cx, id, |tick| TraceRecord::Activate { tick, trigger });
    condition_on_activate(cx, id, trigger);
    if state_of(cx, id, trigger) == Some(STATE_ACTIVE)
        && condition_matches(cx, id, trigger, PROBE, &Parms::Null)
    {
        set_state(cx, id, trigger, STATE_COMPLETE);
        trace(cx, id, |tick| TraceRecord::Fire {
            tick,
            trigger,
            event: PROBE,
        });
        if !run_actions(cx, id, trigger) {
            return;
        }
        rearm_if_repeatable(cx, id, trigger);
    }
    if let Some(s) = script_mut(cx, id)
        && s.triggers
            .get(&trigger)
            .is_some_and(|t| t.state != STATE_COMPLETE)
    {
        s.active_triggers.push(trigger);
    }
}

/// `DeactivateMissionTrigger` (`MissionScript.cpp:426-452`).
pub fn deactivate_trigger(cx: &mut Cx<'_>, id: ControllerId, trigger: u32) {
    let Some(s) = script_mut(cx, id) else { return };
    let at = s.active_triggers.iter().position(|&t| {
        t == trigger
            && s.triggers
                .get(&t)
                .is_some_and(|t| t.state != STATE_COMPLETE)
    });
    let deactivated = if let Some(at) = at {
        if let Some(t) = s.triggers.get_mut(&trigger) {
            t.state = STATE_INACTIVE;
        }
        remove_active(s, at);
        true
    } else {
        false
    };
    s.pending_activations.remove(&trigger);
    if deactivated {
        trace(cx, id, |tick| TraceRecord::Deactivate { tick, trigger });
    }
}

/// `DispatchMissionTriggerEvent` (`MissionScript.cpp:455-514`).
pub fn dispatch(cx: &mut Cx<'_>, id: ControllerId, event: u32, parms: &Parms<'_, '_>) {
    let Some(s) = script_mut(cx, id) else { return };
    s.walks.push(0);
    let depth = s.walks.len() - 1;
    loop {
        let Some(s) = script(cx, id) else { return };
        let Some(&at) = s.walks.get(depth) else { break };
        let Some(&trigger) = s.active_triggers.get(at) else {
            break;
        };
        let Some(t) = s.triggers.get(&trigger) else {
            // An entry whose trigger is gone (D972): passed over.
            advance(cx, id, depth);
            continue;
        };
        if t.events.contains(&event) && t.state == STATE_ACTIVE {
            if condition_matches(cx, id, trigger, event, parms) {
                set_state(cx, id, trigger, STATE_COMPLETE);
                trace(cx, id, |tick| TraceRecord::Fire {
                    tick,
                    trigger,
                    event,
                });
                if !run_actions(cx, id, trigger) {
                    break;
                }
                rearm_if_repeatable(cx, id, trigger);
            }
            if state_of(cx, id, trigger) == Some(STATE_COMPLETE) {
                // The walk's entry leaves the list; the walk stays on the
                // entry after it.
                if let Some(s) = script_mut(cx, id)
                    && let Some(&at) = s.walks.get(depth)
                    && s.active_triggers.get(at) == Some(&trigger)
                {
                    s.active_triggers.remove(at);
                    for (d, cursor) in s.walks.iter_mut().enumerate() {
                        if d != depth && *cursor > at {
                            *cursor -= 1;
                        }
                    }
                }
                continue;
            }
        }
        advance(cx, id, depth);
    }
    if let Some(s) = script_mut(cx, id) {
        s.walks.truncate(depth);
    }
    // The pending activations, front to back.
    while let Some(target) = script_mut(cx, id).and_then(|s| {
        let (&target, _) = s.pending_activations.first_key_value()?;
        s.pending_activations.remove(&target);
        Some(target)
    }) {
        activate_trigger(cx, id, target);
    }
}

fn advance(cx: &mut Cx<'_>, id: ControllerId, depth: usize) {
    if let Some(cursor) = script_mut(cx, id).and_then(|s| s.walks.get_mut(depth)) {
        *cursor += 1;
    }
}

/// `TickMissionTimers` (`MissionScript.cpp:517-537`): every timer counts
/// down; one at or below 0 runs its action and goes. An action whose
/// trigger is gone is dropped without running (D972).
pub fn tick_timers(cx: &mut Cx<'_>, id: ControllerId, dt: f32) {
    let mut i = 0;
    loop {
        let Some(s) = script_mut(cx, id) else { return };
        let Some(timer) = s.timers.get_mut(i) else {
            return;
        };
        timer.remaining -= dt;
        if timer.remaining <= 0.0 {
            let timer = s.timers.remove(i);
            let (trigger, index) = (timer.trigger, timer.index);
            let at = u32::try_from(index).unwrap_or(u32::MAX);
            run_action(cx, id, trigger, index, |tick| TraceRecord::Timer {
                tick,
                trigger,
                index: at,
            });
            continue;
        }
        i += 1;
    }
}

/// `ActivateInitialMissionTriggers` (`MissionStart`): every `InitState =
/// ACTIVE` trigger in id order.
pub fn activate_initial(cx: &mut Cx<'_>, id: ControllerId) {
    let ids: Vec<u32> = script(cx, id)
        .map(|s| s.initially_active.keys().copied().collect())
        .unwrap_or_default();
    for trigger in ids {
        activate_trigger(cx, id, trigger);
    }
}

/// `SetDesiredMissionState` (`MissionScript.cpp:552-558`): a FAIL is
/// dropped while `Field_0x2b0` is set.
pub fn set_desired_state(s: &mut MissionScript, state: i32, mad_lib: u32) {
    if s.field_0x2b0 && state == MISSION_STATE_FAIL {
        return;
    }
    s.mission_state = state;
    s.field_0x2e8 = mad_lib;
}

/// `FMissionObject::HandleMessage` (`MissionObject.cpp:129-142`): a mission
/// that destroyed itself ignores every message; one that asks for its
/// teardown, before or during the message, has it asked of the bus.
pub fn handle_message(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    let Some(s) = script(cx, id) else { return };
    if s.object.destroyed {
        return;
    }
    if !s.object.pending_destroy {
        handle_mission_message(cx, id, message);
        if !script(cx, id).is_some_and(|s| s.object.pending_destroy) {
            return;
        }
    }
    api::controller::destroy(cx, id);
    if let Some(s) = script_mut(cx, id) {
        s.object.destroyed = true;
    }
}

/// The mission's player, when the player has a state.
fn live_player(cx: &Cx<'_>, id: ControllerId) -> Option<u32> {
    let s = script(cx, id)?;
    player_state(cx, s).map(|_| s.object.player)
}

/// `FMissionScript::HandleMissionMessage` (`MissionScript.cpp:1046-1314`).
#[allow(clippy::too_many_lines)]
pub fn handle_mission_message(
    cx: &mut Cx<'_>,
    id: ControllerId,
    message: &mut ControllerMessage<'_>,
) {
    use ControllerMessage as M;
    let Some(player) = live_player(cx, id) else {
        return;
    };
    let event = message.id();
    let mut base_body = true;
    match message {
        M::BaseEnter { player: p, .. } => {
            if *p == player {
                on_base_enter(cx, id, message);
            }
        }
        M::BaseExit { base, player: p } => {
            if *p == player {
                // `OnBaseExit` (`:1358-1394`).
                scenes::on_base_exit(cx, id, *base);
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::CargoScanned { .. }
        | M::CommComplete { .. }
        | M::ObjectDamaged(_)
        | M::TradelaneEnter { .. }
        | M::TradelaneExit { .. }
        | M::RtcDone { .. }
        | M::JumpInComplete { .. }
        | M::LaunchComplete { .. }
        | M::PlayerManeuver { .. }
        | M::JumpgateAct { .. }
        | M::NpcSystemExit { .. }
        | M::EncounterLaunched { .. }
        | M::RumorHeard { .. }
        | M::TetherObject { .. } => dispatch(cx, id, event, &Parms::Message(message)),
        M::PlayerSave { client, file } => {
            // P3.5b's writer, with the case's gates and the mission
            // object's body; nothing else of the mission runs. The
            // mission object's snapshot of a player with a ship comes
            // first (it changes nothing `[MissionState]` writes).
            base_body = false;
            let client = *client;
            let snapshot = client == player
                && api::misc::single_player(cx)
                && script(cx, id).is_some_and(|s| !s.object.trigger_save_pending)
                && spawn::player_ship_id(cx, id) != 0;
            if snapshot {
                spawn::snapshot_mission_objects(cx, id);
            }
            let _ = crate::content::with_instance_taken(cx, id, |cx, instance| {
                if let crate::content::Instance::Mission(s) = instance {
                    let _ = save::player_save(cx, s, client, &mut **file);
                }
            });
        }
        M::PlayerLoad { player: p, reader } => {
            base_body = false;
            let p = *p;
            let steps = crate::content::with_instance_taken(cx, id, |cx, instance| {
                if let crate::content::Instance::Mission(s) = instance {
                    save::player_load(cx, s, p, reader)
                } else {
                    Vec::new()
                }
            })
            .unwrap_or_default();
            // The restore's `ReleaseRtcScripts`: the started cut scenes'
            // bar scenes (P3.5c4); the other steps are P3.5c5's and
            // P3.5d's (D973).
            for step in steps {
                if let save::RestoreStep::ReleaseRtcScript { handle } = step {
                    scenes::release_restored_rtc(cx, id, handle);
                }
            }
        }
        M::PlayerLaunch { player: p, .. } => {
            if *p == player {
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::PlayerLogin { .. } | M::SystemLoaded { .. } | M::LoadGame { .. } => base_body = false,
        M::PlayerLogout { .. } => {
            let gone = !cx.logic.content.state.players.contains_key(&player);
            if let Some(s) = script_mut(cx, id) {
                s.active_triggers.clear();
                s.walks.iter_mut().for_each(|w| *w = 0);
                s.triggers.clear();
                if gone {
                    s.object.pending_destroy = true;
                    s.object.player = 0;
                    s.player = 0;
                    base_body = false;
                }
            }
        }
        M::NpcShipCreated { ship } => {
            let ship = *ship;
            let ours = cx
                .logic
                .content
                .state
                .npc_ships
                .get(&ship)
                .is_some_and(|n| n.controller == id.0);
            if ours {
                spawn::register_and_submit(cx, id, ship);
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::LootAcquired {
            loot, sub_object, ..
        } => {
            let (loot, sub_object) = (*loot, *sub_object);
            dispatch(cx, id, event, &Parms::Message(message));
            if let Some(s) = script_mut(cx, id)
                && let Some(record) = s.object.loot.iter_mut().find(|l| l.space_obj_id == loot)
            {
                record.sub_obj_id = u32::from(sub_object);
            }
        }
        M::GfObjSelect { object, player: p } => {
            if *p == player {
                // `OnCharacterSelect` (`:1397-1427`).
                scenes::on_character_select(cx, id, *object);
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::Heartbeat { elapsed } => {
            let dt = *elapsed;
            if let Some(s) = script_mut(cx, id) {
                s.elapsed_time += dt;
            }
            tick_timers(cx, id, dt);
            dispatch(cx, id, HEARTBEAT, &Parms::Delta(dt));
            dispatch(cx, id, TETHER_OBJECT, &Parms::Null);
        }
        M::AbortMission { player: p, .. } => {
            let p = *p;
            mission_object_body(cx, id, message);
            on_abort(cx, id, p);
            base_body = false;
        }
        M::LocationEnter {
            location,
            player: p,
        } => {
            if *p == player {
                let location = *location;
                let on_base = cx
                    .logic
                    .content
                    .state
                    .players
                    .get(&player)
                    .is_some_and(|s| s.base.is_some());
                if let Some(s) = script_mut(cx, id)
                    && on_base
                {
                    s.current_location = location;
                }
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::LocationExit {
            location,
            player: p,
        } => {
            if *p == player {
                // `OnLocationExit` (`:1430-1475`).
                scenes::on_location_exit(cx, id, *location);
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::MissionResponse {
            offer, player: p, ..
        } => {
            if *p == player && *offer == id.0 {
                on_offer_response(cx, id, message);
            }
        }
        M::PopUpDialog { player: p, button } => {
            if *p == player {
                let button = *button;
                on_pop_up_answer(cx, id, player, button);
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::SpaceEnter { player: p, .. } => {
            let p = *p;
            mission_object_body(cx, id, message);
            if p == player {
                on_space_enter(cx, id, message);
            }
            base_body = false;
        }
        M::SpaceExit { player: p, .. } => {
            let p = *p;
            mission_object_body(cx, id, message);
            if p == player {
                dispatch(cx, id, event, &Parms::Message(message));
            }
            base_body = false;
        }
        M::ObjectDestroyed(_) => on_object_destroyed(cx, id, message),
        M::SystemExit { player: p, .. } => {
            if *p == player {
                dispatch(cx, id, event, &Parms::Message(message));
            }
        }
        M::SystemEnter { .. } => {
            mission_object_body(cx, id, message);
            on_system_enter(cx, id, message);
            base_body = false;
        }
        M::MissionStart => {
            activate_initial(cx, id);
            base_body = false;
        }
        M::DamageReputation {
            provoked,
            attacker,
            victim,
            ..
        } => {
            let live = cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .is_some_and(|s| s.live_mission == id.0);
            if live {
                on_damage_reputation(cx, id, u32::from(*provoked), *attacker, *victim);
            }
            base_body = false;
        }
        M::Msg0x1006 {
            directive_word,
            one,
            ..
        } => {
            if *directive_word == 0
                && let Some(s) = script_mut(cx, id)
            {
                s.field_0x2e8 = if *one == 1 { 0x5462 } else { *one };
            }
        }
        // `OnMessage100b` (`:1671-1724`): the cut scenes; the first
        // objective's resend is P3.5c5's (D973).
        M::PlayerMissionChanged { mission, .. } => {
            scenes::on_mission_changed(cx, id, mission.0);
        }
        _ => {}
    }
    if base_body {
        mission_object_body(cx, id, message);
    }
    // The tail: the NN path and the nags (P3.5c5, D973), then the state.
    apply_desired_state(cx, id);
}

/// `FMissionObject::HandleMissionMessage` (`MissionObject.cpp:1496-1926`):
/// the mission object's cases (P3.5c1's abort and dead players, P3.5c2's
/// objects, jumpers, respawns and snapshot). The random encounters'
/// `NotifyRandEncs` (P3.6d), the group members' cases, the formation
/// leader's objectives and the objective timer of the tail (P3.5c5) wait
/// (D973).
#[allow(clippy::too_many_lines)]
fn mission_object_body(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    use ControllerMessage as M;
    let Some(player) = live_player(cx, id) else {
        return;
    };
    // Every message but `PlayerLogout` and `GroupMemberLeft` first goes to
    // the random encounters (`NotifyRandEncs`, P3.6d).
    if !matches!(message, M::PlayerLogout { .. } | M::GroupMemberLeft { .. }) {
        crate::content::random_encounters::notify_rand_encs(cx, id, message);
    }
    match message {
        M::PlayerLaunch { player: p, .. } => {
            if !api::misc::single_player(cx) || *p != player {
                return;
            }
            let names = script_mut(cx, id)
                .map(|s| std::mem::take(&mut s.object.respawn_solars))
                .unwrap_or_default();
            for name in names {
                spawn::spawn_solar(cx, id, &name, None);
            }
        }
        M::PlayerLogout { player: p } | M::GroupMemberLeft { player: p } => {
            let p = *p;
            // `RemovePlayerById`: the record's objectives are P3.5c5's
            // (D973); the record goes.
            if let Some(s) = script_mut(cx, id) {
                s.object.players.remove(&p);
                if let Some(at) = s.object.dead_players.iter().position(|d| *d == p) {
                    s.object.dead_players.remove(at);
                }
            }
            if p == player {
                // `Slot15` is false: the first remaining record's player,
                // else none.
                if let Some(s) = script_mut(cx, id) {
                    let next = s.object.players.keys().next().copied().unwrap_or(0);
                    s.object.player = next;
                    s.player = next;
                }
            }
            // A group member's `Slot16` is false (the group cases, D973).
            let gone = script(cx, id).is_none_or(|s| player_state(cx, s).is_none());
            if gone && let Some(s) = script_mut(cx, id) {
                s.object.pending_destroy = true;
            }
        }
        M::Heartbeat { elapsed } => spawn::update_jumpers(cx, id, *elapsed),
        M::AbortMission { player: p, .. } => {
            let live = cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .is_some_and(|s| s.live_mission == id.0);
            if *p == player && live {
                on_mission_fail(cx, id);
            }
        }
        M::Msg0x25 { ship, .. } => spawn::on_ship_jump(cx, id, *ship),
        M::JumpInComplete { .. } => spawn::jump_in_complete(cx, id),
        M::Msg0x27 { ship, .. } => {
            let dead =
                crate::content::players::find_by_ship(&cx.logic.content.state.players, *ship);
            if let (Some(dead), Some(s)) = (dead, script_mut(cx, id)) {
                s.object.dead_players.push(dead);
            }
        }
        M::SpaceEnter {
            player: p,
            launch_object,
            ..
        } => {
            if let Some(s) = script_mut(cx, id)
                && let Some(at) = s.object.dead_players.iter().position(|d| d == p)
            {
                s.object.dead_players.remove(at);
            }
            if *p != player || !api::misc::single_player(cx) {
                return;
            }
            let mut keep = true;
            if launch_object & ID_MASK != 0 {
                let kind = api::space_obj::object_type(cx, *launch_object).unwrap_or(0);
                if kind & 0x20 != 0 || kind & 0x100 != 0 {
                    keep = false;
                }
            }
            spawn::respawn_mission_objects(cx, id, keep);
            spawn::apply_vibe_infos(cx, id);
        }
        M::SpaceExit { player: p, .. } => {
            if api::misc::single_player(cx) && *p == player {
                spawn::snapshot_mission_objects(cx, id);
            }
        }
        M::ObjectDestroyed(d) => {
            let (object, system, owner) = (d.id, d.system, d.owner_player);
            spawn::object_destroyed(cx, id, object, system);
            let Some(s) = script(cx, id) else { return };
            if s.object.dead_players.contains(&owner) {
                return;
            }
            let Some(state) = player_state(cx, s) else {
                return;
            };
            let ship = state.ship.as_ref().map_or(0, |p| p.ship);
            // `Slot12` is true; a group member's `Slot13` false; the third
            // test is reached only after the first.
            if s.object.player == owner && ship == object {
                on_mission_fail(cx, id);
            }
        }
        M::SystemEnter {
            system, arrival, ..
        } => spawn::place_jumpers_at_launch(cx, id, *system, *arrival),
        M::Msg0x1006 { directive_word, .. } => {
            if *directive_word == 0 {
                on_mission_fail(cx, id);
            }
        }
        M::NpcShipCreated { ship } => {
            let ours = cx
                .logic
                .content
                .state
                .npc_ships
                .get(ship)
                .is_some_and(|n| n.controller == id.0);
            if ours {
                spawn::npc_ship_created(cx, id, *ship);
            }
        }
        _ => {}
    }
}

/// `FMissionScript::Slot28(Parms)` (`MissionScript.cpp:836-840`): a jumper
/// that arrived dispatches `NpcSystemEnter` with `{ship, system, object}`
/// (`UpdateJumpers` passes two words; the condition reads the third only
/// for the player's ship).
pub fn slot28(cx: &mut Cx<'_>, id: ControllerId, words: [u32; 3]) {
    dispatch(cx, id, NPC_SYSTEM_ENTER, &Parms::NpcEnter(words));
}

/// `OnBaseEnter` (`MissionScript.cpp:1317-1355`): the cut scenes started
/// and the base's ambient scenes made ([`scenes::on_base_enter`]), the
/// objectives' renumber flag cleared (the renumber and the NN path's
/// rebuild are P3.5c5's, D973), then `BaseEnter`.
fn on_base_enter(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    if let ControllerMessage::BaseEnter { base, .. } = *message {
        scenes::on_base_enter(cx, id, base);
    }
    if let Some(s) = script_mut(cx, id)
        && s.field_0x2ec
    {
        s.field_0x2ec = false;
    }
    dispatch(cx, id, message.id(), &Parms::Message(message));
}

/// `OnSpaceEnter` (`MissionScript.cpp:1522-1556`): the dialog speakers'
/// costume hints ([`scenes::send_costume_hints`]), `SpaceEnter`, then the
/// renumber flag cleared (the objectives are P3.5c5's, D973).
fn on_space_enter(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    scenes::send_costume_hints(cx, id);
    dispatch(cx, id, message.id(), &Parms::Message(message));
    if let Some(s) = script_mut(cx, id) {
        s.field_0x2ec = false;
    }
}

/// `OnObjectDestroyed` (`MissionScript.cpp:1559-1579`).
fn on_object_destroyed(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let ControllerMessage::ObjectDestroyed(d) = message else {
        return;
    };
    let object = d.id;
    dispatch(cx, id, message.id(), &Parms::Message(message));
    let cx_ref: &Cx<'_> = cx;
    let at = script(cx_ref, id).and_then(|s| {
        s.friendly_fire_excused
            .iter()
            .position(|label| label_object_id(cx_ref, s, label) == object)
    });
    let Some(s) = script(cx, id) else { return };
    let player = s.object.player;
    let dead = s.object.dead_players.contains(&player);
    let shipless = player_state(cx, s).is_some_and(|p| p.ship.is_none());
    if let Some(s) = script_mut(cx, id) {
        if let Some(at) = at {
            s.friendly_fire_excused.remove(at);
        }
        if !dead && shipless {
            s.object.pending_destroy = true;
        }
    }
}

/// `OnSystemEnter` and `OnSystemEnterPopUp` (`MissionScript.cpp:1582-1615`).
fn on_system_enter(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let ControllerMessage::SystemEnter {
        system,
        from,
        player: p,
        arrival,
        ..
    } = *message
    else {
        return;
    };
    let Some(player) = live_player(cx, id) else {
        return;
    };
    if p == player {
        dispatch(cx, id, message.id(), &Parms::Message(message));
        let ship = cx
            .logic
            .content
            .state
            .players
            .get(&player)
            .and_then(|s| s.ship.as_ref())
            .map(|s| s.ship);
        if let Some(ship) = ship {
            let object = if from & ID_MASK != 0 { arrival } else { 0 };
            dispatch(
                cx,
                id,
                NPC_SYSTEM_ENTER,
                &Parms::NpcEnter([ship, system, object]),
            );
        }
    }
    let Some(s) = script(cx, id) else { return };
    if !s.rep_pop_up_armed || system != s.rep_pop_up_system || p != s.object.player {
        return;
    }
    let player = s.object.player;
    api::player::pop_up_dialog(cx, player, &FmtStr::new(0x54c4), &FmtStr::new(0x54c5), 5);
    if let Some(s) = script_mut(cx, id) {
        s.rep_pop_up_shown = true;
    }
}

/// The `PopUpDialog` case's answer to the reputation pop-up
/// (`MissionScript.cpp:1204-1222`).
fn on_pop_up_answer(cx: &mut Cx<'_>, id: ControllerId, player: u32, button: u32) {
    let Some(s) = script(cx, id) else { return };
    if !s.rep_pop_up_shown {
        return;
    }
    if button & 1 != 0 {
        let groups = s.rep_pop_up_groups.clone();
        let rep = api::player::rep(cx, player).unwrap_or(-1);
        for group in groups {
            let _ = api::reputation::set_reputation(cx, rep, group, 0.6);
        }
        if let Some(s) = script_mut(cx, id) {
            s.rep_pop_up_armed = false;
        }
    }
    if let Some(s) = script_mut(cx, id) {
        s.rep_pop_up_shown = false;
    }
}

/// `OnOfferResponse` (`MissionScript.cpp:1478-1519`): a refusal only
/// dispatches; an acceptance shows the empty type-10 message, dispatches,
/// resets the pop-up, sets the clamps and joins the player's group to the
/// mission. Story's event and the end of another live mission are P3.5d's
/// and P3.5c5's (D973).
fn on_offer_response(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let ControllerMessage::MissionResponse { accepted, .. } = *message else {
        return;
    };
    let Some(player) = live_player(cx, id).filter(|_| accepted) else {
        dispatch(cx, id, message.id(), &Parms::Message(message));
        return;
    };
    let _ = api::player::display_mission_message(cx, player, &FmtStr::new(0), 0xa, true);
    dispatch(cx, id, message.id(), &Parms::Message(message));
    let Some(s) = script_mut(cx, id) else { return };
    s.rep_pop_up_shown = false;
    s.rep_pop_up_armed = false;
    s.rep_pop_up_system = 0;
    s.rep_pop_up_groups.clear();
    let notify_arg = s.field_0x2c4;
    let clamps = &mut cx.logic.content.state.clamps;
    clamps.attack_clamp = 1;
    clamps.encounter_enabled = 0;
    clamps.scan_clamp = 1;
    crate::content::players::join_mission_as_group(cx, player, id.0, 0, 0, true, notify_arg);
}

/// `OnMessage24` (`MissionScript.cpp:1618-1631`), after the mission
/// object's `AbortMission`.
fn on_abort(cx: &mut Cx<'_>, id: ControllerId, aborting: u32) {
    let Some(s) = script(cx, id) else { return };
    let player = s.object.player;
    let Some(state) = player_state(cx, s) else {
        return;
    };
    if aborting != player || state.live_mission != id.0 {
        return;
    }
    let text = FmtStr::new(s.field_0x2e8);
    let _ = api::player::display_mission_message(cx, player, &text, 0, true);
    if let Some(s) = script_mut(cx, id) {
        s.object.pending_destroy = true;
    }
}

/// `OnMessage1005` (`MissionScript.cpp:1634-1668`): the player's ship
/// provoking a victim that no friendly-fire label excuses fails the
/// mission when its attitude towards the ship is above -0.2.
fn on_damage_reputation(cx: &mut Cx<'_>, id: ControllerId, kind: u32, ship: u32, victim: u32) {
    let Some(s) = script(cx, id) else { return };
    let Some(own) = player_state(cx, s)
        .and_then(|p| p.ship.as_ref())
        .map(|s| s.ship)
    else {
        return;
    };
    if ship != own || kind != 1 {
        return;
    }
    if s.friendly_fire_excused
        .iter()
        .any(|label| label_object_id(cx, s, label) == victim)
    {
        return;
    }
    let victim_rep = api::space_obj::rep(cx, victim).unwrap_or(0);
    // ShipRep is not initialised in the C++; the getter writes it for a
    // live ship, which the player's is (D976).
    let ship_rep = api::space_obj::rep(cx, ship).unwrap_or(0);
    let attitude = api::reputation::attitude(cx, victim_rep, ship_rep).unwrap_or(0.0);
    let Some(s) = script_mut(cx, id) else { return };
    if attitude > 0.2 {
        set_desired_state(s, MISSION_STATE_FAIL, 0x5460);
    } else if attitude >= -0.2 {
        set_desired_state(s, MISSION_STATE_FAIL, 0x5463);
    }
}

/// `ApplyDesiredMissionState` (`MissionScript.cpp:846-865`). Story's
/// `OnMissionEvent` after a success is P3.5d's (D973).
pub fn apply_desired_state(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script_mut(cx, id) else { return };
    let (desired, previous) = (s.mission_state, s.previous_state);
    if desired == previous {
        return;
    }
    s.previous_state = desired;
    trace(cx, id, |tick| TraceRecord::State {
        tick,
        from: previous,
        to: desired,
    });
    if desired == MISSION_STATE_FAIL {
        on_mission_fail(cx, id);
    } else if desired == MISSION_STATE_SUCCEED {
        on_mission_succeed(cx, id);
    }
}

/// `FMissionScript::OnMissionSucceed` (`MissionScript.cpp:870-885`) and the
/// mission object's (`MissionObject.cpp:1932-1945`): the success message
/// and the mission objects' teardown, then `EndMission`. The completion
/// count, the reward, the objectives and the end of the live mission are
/// P3.5c5's (D973).
fn on_mission_succeed(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script_mut(cx, id) else { return };
    let player = s.object.player;
    if s.field_0x2e8 != 0 && cx.logic.content.state.players.contains_key(&player) {
        let Some(s) = script_mut(cx, id) else { return };
        if s.field_0x2e8 == 1 {
            s.field_0x2e8 = 0x4cf;
        }
        let text = FmtStr::new(s.field_0x2e8);
        let _ = api::player::display_mission_message(cx, player, &text, 1, true);
        // `CountRandomMissionCompleted(7)` is P3.5c5's (D973).
        spawn::destroy_mission_objects(cx, id);
    }
    end_mission(cx, id);
}

/// `FMissionScript::OnMissionFail` (`MissionScript.cpp:888-905`) and the
/// mission object's (`MissionObject.cpp:1950-1961`): the failure message,
/// the NPCs muted, the mission objects' teardown, then `EndMission` (the
/// rest as for the success, D973).
fn on_mission_fail(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script_mut(cx, id) else { return };
    if s.field_0x2e8 == 1 {
        s.field_0x2e8 = 0x412;
    }
    let (player, text) = (s.object.player, FmtStr::new(s.field_0x2e8));
    let npcs: Vec<_> = s.object.npc_objects.values().copied().collect();
    let _ = api::player::display_mission_message(cx, player, &text, 0, true);
    for key in npcs {
        if let Some(npc) = cx.logic.content.state.npc_ships.get_mut(&key) {
            npc.muted = true;
        }
    }
    spawn::destroy_mission_objects(cx, id);
    end_mission(cx, id);
}

/// `FMissionScript::EndMission` (`MissionScript.cpp:908-956`) and the
/// mission object's (`MissionObject.cpp:1292-1322`). The nags and the
/// players' objectives are P3.5c5's (D973).
pub fn end_mission(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script_mut(cx, id) else { return };
    s.timers.clear();
    let player = s.object.player;
    // The player's ship id is read without a test of the state (D976).
    let ship = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(0, |p| p.ship.as_ref().map_or(0, |s| s.ship));
    if ship & ID_MASK == 0 {
        let _ = api::ai::lock_maneuvers(cx, ship, false);
    }
    scenes::release_rtc_scripts(cx, id);
    let Some(s) = script_mut(cx, id) else { return };
    s.friendly_fire_excused.clear();
    s.object.rand_encs.clear();
    let content = &mut cx.logic.content.state;
    content.mission_dialogs.clear();
    content.clamps.attack_clamp = 0;
    content.clamps.scan_clamp = 0;
    content.clamps.encounter_enabled = 1;
    content.clamps.population_enabled = 1;
    content.exclusion_spheres.clear();
    content.clamps.hostile_clamp = 1;
    let has_state = if let Some(state) = content.players.get_mut(&player) {
        state.set_dock_access(true, &[]);
        state.set_tradelane_access(true, &[]);
        true
    } else {
        false
    };
    if has_state {
        let _ = api::audio::cancel_music(cx, player);
    }
    let Some(s) = script_mut(cx, id) else { return };
    s.object.invulnerables.clear();
    s.object.decloaked.clear();
    s.object.mark_obj_names.clear();
    // FMissionObject::EndMission.
    s.object.players.clear();
    s.object.title = 0;
    s.object.offer = 0;
    s.object.npcs.clear();
    s.object.npc_objects.clear();
    s.object.npc_names.clear();
    s.object.ships.clear();
    s.object.obj_lists.clear();
    spawn::release_obj_lists(cx, id);
    let Some(s) = script_mut(cx, id) else { return };
    s.object.meta_behaviors.clear();
    s.object.solars.clear();
    s.object.rand_encs.clear();
    s.object.pending_destroy = true;
}

/// The missions' destructors (P3.5c4): `~FMissionScript`
/// (`MissionScript.cpp:683-720`: the cut scenes and ambient scenes,
/// [`scenes::destruct`]; its nags are P3.5c5's, D973), then
/// `~FMissionObject` (`MissionObject.cpp:107-125`: the random encounters,
/// the players' records, whose objectives' send is P3.5c5's, and every
/// jumper out of every system deleted), then the members' (the respawn
/// records' guides released).
pub fn destruct(cx: &mut Cx<'_>, id: ControllerId) {
    scenes::destruct(cx, id);
    let Some(s) = script_mut(cx, id) else { return };
    s.object.rand_encs.clear();
    s.object.players.clear();
    let gone: Vec<_> = s
        .object
        .jumpers
        .iter_mut()
        .filter(|j| j.state != 0)
        .map(|j| {
            j.state = 0;
            j.npc
        })
        .collect();
    for npc in gone {
        crate::content::npc_ships::destroy(cx, npc);
    }
    spawn::clear_respawn_ships(cx, id);
    spawn::clear_rand_enc_respawns(cx, id);
}

/// Turns the mission's trace on, from tick 0.
pub fn start_trace(cx: &mut Cx<'_>, id: ControllerId) {
    if let Some(s) = script_mut(cx, id) {
        s.trace = Some(super::trace::Trace::new());
    }
}

/// The session's `final` record: the active triggers and the pending
/// activations.
pub fn finish_trace(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let active = s.active_triggers.clone();
    let pending: Vec<u32> = s.pending_activations.keys().copied().collect();
    trace(cx, id, |tick| TraceRecord::Final {
        tick,
        active,
        pending,
    });
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use game_sim::data::config::GameData;
    use game_sim::object::World;
    use game_sim::object::world::WorldConfig;
    use neuron_core::id::create_id;
    use neuron_core::ini::IniReader;

    use super::*;
    use crate::content::Instance;
    use crate::content::mission::action::Action;
    use crate::content::players::PlayerState;
    use crate::server::{ControllerKind, CreateParams, Logic, RecordingSink};

    const ID: ControllerId = ControllerId(0x00ff_0001);

    /// A mission of `text`'s triggers for player 7, who has a state and no
    /// ship, with its trace on; `run` gets the context.
    fn with_mission(text: &str, run: impl FnOnce(&mut Cx<'_>)) {
        let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        let state = PlayerState::new(&cx, 7);
        cx.logic.content.state.players.insert(7, state);
        let params = CreateParams::StoryMission {
            player: 7,
            story: ControllerId::BROADCAST,
        };
        let mut script =
            super::super::construct_script(&mut cx, ID, ControllerKind::GenericMission, &params);
        let mut reader = IniReader::new();
        reader.open_memory(text.as_bytes());
        super::super::load_mission(&mut cx, &mut reader, &mut script);
        script.trace = Some(super::super::trace::Trace::new());
        cx.logic
            .content
            .instances
            .insert(ID, Instance::Mission(Box::new(script)));
        run(&mut cx);
    }

    fn send(cx: &mut Cx<'_>, tick: u32, mut message: ControllerMessage<'_>) {
        if let Some(t) = script_mut(cx, ID).and_then(|s| s.trace.as_mut()) {
            t.tick = tick;
        }
        handle_message(cx, ID, &mut message);
    }

    fn records(cx: &Cx<'_>) -> Vec<TraceRecord> {
        script(cx, ID)
            .and_then(|s| s.trace.as_ref())
            .map(|t| t.records.clone())
            .unwrap_or_default()
    }

    fn fires(cx: &Cx<'_>, name: &[u8]) -> Vec<u32> {
        let trigger = create_id(name);
        records(cx)
            .into_iter()
            .filter_map(|r| match r {
                TraceRecord::Fire {
                    tick, trigger: t, ..
                } if t == trigger => Some(tick),
                _ => None,
            })
            .collect()
    }

    const ENGINE: &str = "\
[Trigger]
nickname = start
InitState = ACTIVE
Cnd_True = no_params
Act_ActTrig = slow
Act_ActTrig = fast

[Trigger]
nickname = slow
Cnd_Timer = 1
Act_DeactTrig = watcher
Act_ChangeState = SUCCEED

[Trigger]
nickname = fast
Cnd_Timer = 0.5
repeatable = true
Act_DebugMsg = again

[Trigger]
nickname = watcher
InitState = ACTIVE
Cnd_SpaceEnter = no_params
Act_DebugMsg = never
";

    #[test]
    fn the_engine_fires_rearms_deactivates_and_ends_the_mission() {
        with_mission(ENGINE, |cx| {
            send(cx, 0, ControllerMessage::MissionStart);
            // The probe fires the initial trigger; its activations wait for
            // the next dispatch.
            assert_eq!(fires(cx, b"start"), [0]);
            let s = script(cx, ID).unwrap();
            assert_eq!(s.active_triggers, [create_id(b"watcher")]);
            assert_eq!(s.pending_activations.len(), 2);
            for tick in 1..=6 {
                send(cx, tick, ControllerMessage::Heartbeat { elapsed: 0.25 });
            }
            // The first heartbeat drains them; the timers count from the
            // second. The repeatable one fires every second heartbeat, its
            // walk not upset by the other's deactivation.
            assert_eq!(fires(cx, b"fast"), [3, 5]);
            assert_eq!(fires(cx, b"slow"), [5]);
            let rec = records(cx);
            assert!(rec.contains(&TraceRecord::Rearm {
                tick: 3,
                trigger: create_id(b"fast")
            }));
            assert!(rec.contains(&TraceRecord::Deactivate {
                tick: 5,
                trigger: create_id(b"watcher")
            }));
            assert!(rec.contains(&TraceRecord::State {
                tick: 5,
                from: 2,
                to: MISSION_STATE_SUCCEED
            }));
            // The success ended the mission, which asked for its teardown
            // and ignores what follows.
            let s = script(cx, ID).unwrap();
            assert!(s.object.destroyed);
            let before = records(cx).len();
            send(cx, 7, ControllerMessage::Heartbeat { elapsed: 0.25 });
            assert_eq!(records(cx).len(), before);
        });
    }

    #[test]
    fn a_delayed_dialog_runs_from_the_timer_list() {
        let text = "\
[Trigger]
nickname = talk
InitState = ACTIVE
Cnd_True = no_params
Act_StartDialog = some_dialog, 2
";
        with_mission(text, |cx| {
            send(cx, 0, ControllerMessage::MissionStart);
            let s = script(cx, ID).unwrap();
            assert_eq!(s.timers.len(), 1);
            let Some(Action::StartDialog(a)) = s.triggers[&create_id(b"talk")].actions.first()
            else {
                panic!("the dialog action");
            };
            // The flag the action keeps in itself is written back.
            assert!(!a.delay_pending);
            send(cx, 1, ControllerMessage::Heartbeat { elapsed: 1.0 });
            send(cx, 2, ControllerMessage::Heartbeat { elapsed: 1.0 });
            assert!(records(cx).contains(&TraceRecord::Timer {
                tick: 2,
                trigger: create_id(b"talk"),
                index: 0
            }));
            let s = script(cx, ID).unwrap();
            assert!(s.timers.is_empty());
            let Some(Action::StartDialog(a)) = s.triggers[&create_id(b"talk")].actions.first()
            else {
                panic!("the dialog action");
            };
            assert!(a.delay_pending, "the delay is armed again for the next run");
        });
    }

    #[test]
    fn the_gate_locks_and_marks_of_p3_5c2_have_their_effect() {
        let text = "\
[Trigger]
nickname = lock
InitState = ACTIVE
Cnd_True = no_params
Act_LockDock = Everyone, some_gate, lock
Act_MarkObj = nothing_here, 1
Act_Invulnerable = Player, true
";
        with_mission(text, |cx| {
            send(cx, 0, ControllerMessage::MissionStart);
            let gate = create_id(b"some_gate");
            let locks = &cx.logic.content.state.gate_locks;
            assert!(locks.is_locked(7, gate));
            assert!(locks.is_npc_locked(gate));
            // No label at or after the key: the label's object (none) is
            // marked, and the name is listed for a later spawn.
            let s = script(cx, ID).unwrap();
            assert_eq!(s.object.mark_obj_names, [b"nothing_here".to_vec()]);
        });
        let text = "\
[Trigger]
nickname = unlock
InitState = ACTIVE
Cnd_True = no_params
Act_LockDock = Player, some_gate, lock
Act_LockDock = Player, some_gate, unlock
";
        with_mission(text, |cx| {
            send(cx, 0, ControllerMessage::MissionStart);
            let gate = create_id(b"some_gate");
            assert!(!cx.logic.content.state.gate_locks.is_locked(7, gate));
        });
    }

    #[test]
    fn a_logout_tears_the_triggers_down() {
        with_mission(ENGINE, |cx| {
            send(cx, 0, ControllerMessage::MissionStart);
            send(cx, 1, ControllerMessage::PlayerLogout { player: 7 });
            let s = script(cx, ID).unwrap();
            assert!(s.triggers.is_empty() && s.active_triggers.is_empty());
            // The player still has a state, but the mission object's case
            // drops the player's record and takes the next one, of which
            // there is none (`SetPlayer(0)`): the mission asks for its
            // teardown (P3.5c2).
            assert_eq!(s.object.player, 0);
            assert!(s.object.destroyed);
        });
    }
}
