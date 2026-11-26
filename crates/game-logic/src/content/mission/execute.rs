//! The actions' run time (`Content/MissionAct.cpp`, TU21; work package
//! P3.5c): `Act::Execute` of each `Act_*` class.
//!
//! P3.5c1 ports group A, the script's own state, Content's clamps and the
//! player's settings (`MissionAct.cpp:107-1310`, `:4694-4708`):
//!
//! - the triggers and the mission state: `Act_ActTrig`, `Act_DeactTrig`,
//!   `Act_ChangeState`, `Act_Save`, `Act_DebugMsg`, and the delay of
//!   `Act_StartDialog` (its timer; the dialog itself is P3.5c4's);
//! - the clamps: `Act_GCSClamp`, `Act_RandomPop`, `Act_RandomPopSphere`,
//!   `Act_RpopAttClamp`, `Act_RpopTLAttacksEnabled`, `Act_HostileClamp`,
//!   `Act_NagClamp`, `Act_DisableEnc`, `Act_DisableFriendlyFire`;
//! - the player: `Act_PlayerCanDock`, `Act_PlayerCanTradelane`,
//!   `Act_PlayMusic`, `Act_PlaySoundEffect`, `Act_NNIds`, `Act_PlayNN`,
//!   `Act_AdjAcct`, `Act_ForceLand`, `Act_SetInitialPlayerPos`,
//!   `Act_EnableManeuver`, `Act_LockManeuvers`, `Act_PlayerEnemyClamp`,
//!   `Act_DisableTradelane`.
//!
//! P3.5c2 ports the 22 classes of the mission object's spawns and objects
//! ([`objects`]): `Act_SpawnShip`, `Act_SpawnShipRel`, `Act_SpawnFormation`,
//! `Act_SpawnSolar`, `Act_SpawnLoot`, `Act_Destroy`, `Act_Jumper`,
//! `Act_RelocateShip`, `Act_RelocateForm`, `Act_SetOrient`,
//! `Act_MovePlayer`, `Act_SetShipAndLoadout`, `Act_RemoveCargo`,
//! `Act_AdjHealth`, `Act_LightFuse`, `Act_Invulnerable`, `Act_Cloak`,
//! `Act_MarkObj`, `Act_SetLifeTime`, `Act_LockDock`, `Act_DockRequest` and
//! `Act_PobjIdle`.
//!
//! P3.5c3 ports the 13 AI and reputation classes ([`ai_rep`]):
//! `Act_GiveObjList`, `Act_GiveMB`, `Act_SetPriority`, `Act_PilotParams`,
//! `Act_SetFlee`, `Act_PlayerForm`, `Act_SetVibe`, `Act_SetVibeLbl`,
//! `Act_SetVibeLblToShip`, `Act_SetVibeShipToLbl`,
//! `Act_SetVibeOfferBaseHack`, `Act_SetRep` and `Act_RepChangeRequest`,
//! with the script's `SetObjectVibe` ([`set_object_vibe`]); the
//! `[ObjList]` directives they hand out run through
//! [`super::obj_list_execute`].
//!
//! The other 23 classes (and `Act_StartDialog` but for its delay) run
//! without an effect until their increment (D970): P3.5c4 the comms,
//! cameras, cut scenes, ambient scenes and offers, P3.5c5 the objectives,
//! nags and the NN path. The trace records each of them as it runs, as the
//! C++'s does.
//!
//! An action is cloned out of its trigger before it runs and written back
//! after it (the engine, [`super::engine`]): `Act_StartDialog` keeps its
//! delay flag in itself (`MissionAct.cpp:4697-4708`), and `Act_SpawnSolar`
//! keeps the record's orientation it read (`:4560-4566`).

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::c_str;
use neuron_server::controller::ControllerId;

use super::action::Action;
use super::engine::{self, MissionTimer};
use super::runtime::{ID_MASK, player_state, script, script_mut};
use crate::api;
use crate::api::audio::Tryptich;
use crate::server::Cx;

mod ai_rep;
mod objects;

pub use ai_rep::set_object_vibe;

/// The actions P3.5c1 to P3.5c3 run with their effect; the rest run
/// without one until their increment (D970).
#[must_use]
pub fn is_ported(action: &Action) -> bool {
    matches!(
        action,
        Action::ActTrig(_)
            | Action::DeactTrig(_)
            | Action::ChangeState(_)
            | Action::Save(_)
            | Action::DebugMsg(_)
            | Action::GcsClamp(_)
            | Action::RandomPop(_)
            | Action::RandomPopSphere(_)
            | Action::RpopAttClamp(_)
            | Action::RpopTlAttacksEnabled(_)
            | Action::HostileClamp(_)
            | Action::NagClamp(_)
            | Action::DisableEnc(_)
            | Action::DisableFriendlyFire(_)
            | Action::PlayerCanDock(_)
            | Action::PlayerCanTradelane(_)
            | Action::PlayMusic(_)
            | Action::PlaySoundEffect(_)
            | Action::NnIds(_)
            | Action::PlayNn(_)
            | Action::AdjAcct(_)
            | Action::ForceLand(_)
            | Action::SetInitialPlayerPos(_)
            | Action::EnableManeuver(_)
            | Action::LockManeuvers(_)
            | Action::PlayerEnemyClamp(_)
            | Action::DisableTradelane(_)
            // P3.5c2.
            | Action::SpawnShip(_)
            | Action::SpawnShipRel(_)
            | Action::SpawnFormation(_)
            | Action::SpawnSolar(_)
            | Action::SpawnLoot(_)
            | Action::Destroy(_)
            | Action::Jumper(_)
            | Action::RelocateShip(_)
            | Action::RelocateForm(_)
            | Action::SetOrient(_)
            | Action::MovePlayer(_)
            | Action::SetShipAndLoadout(_)
            | Action::RemoveCargo(_)
            | Action::AdjHealth(_)
            | Action::LightFuse(_)
            | Action::Invulnerable(_)
            | Action::Cloak(_)
            | Action::MarkObj(_)
            | Action::SetLifeTime(_)
            | Action::LockDock(_)
            | Action::DockRequest(_)
            | Action::PobjIdle(_)
            // P3.5c3.
            | Action::GiveObjList(_)
            | Action::GiveMb(_)
            | Action::SetPriority(_)
            | Action::PilotParams(_)
            | Action::SetFlee(_)
            | Action::PlayerForm(_)
            | Action::SetVibe(_)
            | Action::SetVibeLbl(_)
            | Action::SetVibeLblToShip(_)
            | Action::SetVibeShipToLbl(_)
            | Action::SetVibeOfferBaseHack(_)
            | Action::SetRep(_)
            | Action::RepChangeRequest(_)
    )
}

/// The mission's player and the space object of the player's ship (0
/// without one); `None` without the player's state.
fn player_ship(cx: &Cx<'_>, id: ControllerId) -> Option<(u32, u32)> {
    let s = script(cx, id)?;
    let state = player_state(cx, s)?;
    Some((
        state.player,
        state.ship.as_ref().map_or(0, |ship| ship.ship),
    ))
}

/// `Act::Execute` of the action at `index` of trigger `trigger` of mission
/// `id` (the action's `TriggerId`), on the clone the engine took. The C++
/// result (always true but for `Act_DockRequest` without a player and the
/// maneuver and dialog calls' results) is returned and, as in the C++,
/// unused.
#[allow(clippy::too_many_lines)]
pub fn execute(
    cx: &mut Cx<'_>,
    id: ControllerId,
    trigger: u32,
    index: usize,
    action: &mut Action,
) -> bool {
    let Some(player) = script(cx, id).map(|s| s.object.player) else {
        return true;
    };
    match action {
        Action::ActTrig(a) => {
            // `insert`: a target already pending keeps its first source.
            if let Some(s) = script_mut(cx, id) {
                s.pending_activations
                    .entry(a.target_trigger_id)
                    .or_insert(trigger);
            }
            true
        }
        Action::DeactTrig(a) => {
            engine::deactivate_trigger(cx, id, a.target_trigger_id);
            true
        }
        Action::ChangeState(a) => {
            if let Some(s) = script_mut(cx, id) {
                engine::set_desired_state(s, a.state, a.field_0x10);
            }
            true
        }
        Action::Save(a) => {
            if let Some(s) = script_mut(cx, id) {
                s.object.trigger_save_pending = true;
                s.save_trigger_id = a.save_trigger_id;
            }
            let _ = api::player::display_mission_message(cx, player, &FmtStr::new(0x762), 7, true);
            api::misc::save(cx, player, a.save_type);
            true
        }
        Action::DebugMsg(_) => true,
        Action::StartDialog(a) => {
            if a.delay_pending {
                if let Some(s) = script_mut(cx, id) {
                    s.timers.push(MissionTimer {
                        remaining: a.delay_seconds as f32,
                        trigger,
                        index,
                    });
                }
                a.delay_pending = false;
                return true;
            }
            // StartMissionDialog: the comm and the dialog's speakers come
            // with P3.5c4 over P3.6b's comm system (D971).
            if a.delay_seconds != -1 {
                a.delay_pending = true;
            }
            true
        }
        Action::GcsClamp(a) => {
            cx.logic.content.state.clamps.gcs_clamp_disabled = u8::from(!a.clamp);
            true
        }
        Action::RandomPop(a) => {
            cx.logic.content.state.clamps.population_enabled = u8::from(a.enabled);
            true
        }
        Action::RandomPopSphere(a) => {
            let sphere = [a.center.x, a.center.y, a.center.z, a.radius];
            match a.mode {
                0 => crate::content::population::remove_exclusion_sphere(cx, sphere),
                1 => crate::content::population::add_exclusion_sphere(cx, sphere),
                _ => {}
            }
            true
        }
        Action::RpopAttClamp(a) => {
            cx.logic.content.state.clamps.attack_clamp = u8::from(a.clamp);
            true
        }
        Action::RpopTlAttacksEnabled(a) => {
            cx.logic.content.state.clamps.encounter_enabled = u8::from(a.enabled);
            true
        }
        Action::HostileClamp(a) => {
            cx.logic.content.state.clamps.hostile_clamp = u8::from(a.clamp);
            true
        }
        Action::NagClamp(a) => {
            let keys: Vec<_> = script(cx, id)
                .map(|s| s.object.npc_objects.values().copied().collect())
                .unwrap_or_default();
            for key in keys {
                if let Some(npc) = cx.logic.content.state.npc_ships.get_mut(&key) {
                    npc.muted = a.clamp;
                }
            }
            if let Some(s) = script_mut(cx, id) {
                s.nag_clamp = a.clamp;
            }
            true
        }
        Action::DisableEnc(a) => {
            if let Some(s) = script_mut(cx, id) {
                for enc in &mut s.object.rand_encs {
                    if c_str(&enc.nickname) == c_str(&a.encounter) {
                        enc.enabled = false;
                    }
                }
            }
            true
        }
        Action::DisableFriendlyFire(a) => {
            if let Some(s) = script_mut(cx, id) {
                for label in &a.labels {
                    if !s
                        .friendly_fire_excused
                        .iter()
                        .any(|known| c_str(known) == c_str(label))
                    {
                        s.friendly_fire_excused.push(label.clone());
                    }
                }
            }
            true
        }
        Action::PlayerCanDock(a) => {
            if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
                state.set_dock_access(a.can_dock, &a.exceptions);
            }
            true
        }
        Action::PlayerCanTradelane(a) => {
            if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
                state.set_tradelane_access(a.can_tradelane, &a.exceptions);
            }
            true
        }
        Action::PlayMusic(a) => {
            if a.cancel {
                let _ = api::audio::cancel_music(cx, player);
                return true;
            }
            let track = Tryptich {
                space: a.music[0],
                danger: a.music[1],
                battle: a.music[2],
                override_music: a.music[3],
                flag: u8::from(a.field_10),
                fade: a.fade,
            };
            let _ = api::audio::set_music(cx, player, &track);
            true
        }
        Action::PlaySoundEffect(a) => {
            let _ = api::audio::play_sound_effect(cx, player, a.sound_id);
            true
        }
        Action::NnIds(a) => {
            let text = FmtStr::new(a.text_id.cast_unsigned());
            let kind = a.message_type.cast_unsigned();
            let _ = api::player::display_mission_message(cx, player, &text, kind, true);
            true
        }
        Action::PlayNn(a) => {
            let _ = api::player::send_nn_message(cx, player, a.message_id);
            true
        }
        Action::AdjAcct(a) => {
            if let Some((owner, _)) = player_ship(cx, id) {
                let _ = api::player::adjust_cash(cx, owner, a.amount);
            }
            true
        }
        Action::ForceLand(a) => {
            api::player::force_land(cx, player, a.base_id);
            true
        }
        Action::SetInitialPlayerPos(a) => {
            if api::player::set_initial_pos(cx, player, a.position).is_ok() {
                let _ = api::player::set_initial_orient(cx, player, &a.orientation);
            }
            true
        }
        Action::EnableManeuver(a) => {
            // The cached player state, not looked up (D976).
            let ship = player_ship(cx, id).map_or(0, |(_, ship)| ship);
            api::ai::enable_maneuver(cx, ship, a.maneuver_id, a.enable)
        }
        Action::LockManeuvers(a) => {
            let ship = player_ship(cx, id).map_or(0, |(_, ship)| ship);
            api::ai::lock_maneuvers(cx, ship, a.lock)
        }
        Action::PlayerEnemyClamp(a) => {
            if let Some((_, ship)) = player_ship(cx, id)
                && ship & ID_MASK != 0
            {
                let _ = api::ai::set_player_enemy_clamp(cx, ship, a.min_clamp, a.max_clamp);
            }
            true
        }
        Action::DisableTradelane(a) => {
            let _ = api::space_obj::drain_shields(cx, a.ring_id);
            true
        }
        Action::GiveObjList(_)
        | Action::GiveMb(_)
        | Action::SetPriority(_)
        | Action::PilotParams(_)
        | Action::SetFlee(_)
        | Action::PlayerForm(_)
        | Action::SetVibe(_)
        | Action::SetVibeLbl(_)
        | Action::SetVibeLblToShip(_)
        | Action::SetVibeShipToLbl(_)
        | Action::SetVibeOfferBaseHack(_)
        | Action::SetRep(_)
        | Action::RepChangeRequest(_) => ai_rep::execute(cx, id, action),
        _ => objects::execute(cx, id, action),
    }
}
