//! Character selection (work package P3.2): `PlayerData_SelectCharacter`
//! (`Server/server_classes.cpp:2848`), `PlayerData_LoadSelectedCharacter`
//! and `Server_LoadSinglePlayerState` (`Server/server_savegame.cpp:971`,
//! `Server/IServerImpl.cpp:120`).
//!
//! On a multiplayer server, re-selecting while a character is selected
//! first takes the old one out of its system and base (`PlayerDB_LeaveSystem`,
//! `PlayerDB_LeaveBase`, the `SystemExit` broadcast), ends its trade and
//! takes it out of its group. Those need the system and base databases
//! (P3.3, P3.4), trades and groups, and are not done yet; the rest of the
//! re-selection (the `PlayerLogout` and `PlayerLogin` broadcasts and the
//! player-list removals) is. The groups the player list reports are 0
//! until groups are ported.

use neuron_core::ini::IniReader;
use neuron_server::controller::ControllerId;

use crate::character_file::SavedCharacter;
use crate::client;
use crate::events::ControllerMessage;
use crate::login::ServerFiles;
use crate::player::{self, CharacterId};
use crate::server::Cx;

/// The name the player list shows for a player without a vibe.
const BAD_NAME: &str = ">>>BAD   NAME<<<";

/// `PlayerData_SelectCharacter(slot, id, session)`: `false` for a
/// character the slot does not have. On a multiplayer server the slot
/// takes the character and every valid player (with a character, on a
/// dedicated server) learns of it, and it of them; then
/// [`load_selected_character`].
pub fn select_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    id: &CharacterId,
) -> bool {
    let Some(record) = cx
        .logic
        .players
        .get(session)
        .and_then(|slot| slot.saved_characters.get(id))
        .cloned()
    else {
        return false;
    };
    if !cx.world.config().single_player {
        let own_group = 0;
        let selected = cx
            .logic
            .players
            .get(session)
            .map(|slot| slot.selected_character.clone())
            .unwrap_or_default();
        if selected.is_valid() && *id != selected {
            broadcast(cx, &mut ControllerMessage::PlayerLogout { player: session });
            let others: Vec<u32> = cx
                .logic
                .players
                .active_ids()
                .filter(|&other| other != session)
                .collect();
            for other in others {
                client::player_list_remove(cx, other, session);
            }
            broadcast(cx, &mut ControllerMessage::PlayerLogin { player: session });
        }
        if let Some(slot) = cx.logic.players.get_mut(session) {
            slot.selected_character = id.clone();
        }
        client::player_list_clear(cx, session);
        let max = cx.logic.players.max_players() as u32;
        for other in 1..=max {
            if !cx.logic.players.is_valid(other) {
                continue;
            }
            let Some(other_slot) = cx.logic.players.get(other) else {
                continue;
            };
            if cx.logic.multiplayer_server && !other_slot.selected_character.is_valid() {
                continue;
            }
            let (vibe, system) = (other_slot.rep_vibe, other_slot.system_id);
            let (vibes, _) = cx.world.vibes();
            let rank = match vibes.rank(vibe) {
                (0, rank) => rank,
                _ => 0,
            };
            let other_group = 0;
            if other != session {
                let name = vibes.name(vibe).map_or_else(
                    || BAD_NAME.encode_utf16().collect(),
                    |(_, _, display)| display,
                );
                client::player_list_add(cx, session, &name, other, true);
                client::misc_obj_update_player_group(cx, session, other, other_group);
                client::misc_obj_update_rank(cx, session, other, rank as i32);
                client::misc_obj_update_system(cx, session, other, system);
            }
            client::player_list_add(cx, other, &record.name, session, false);
            client::misc_obj_update_player_group(cx, other, session, own_group);
            client::misc_obj_update_rank(cx, other, session, record.rank as i32);
            client::misc_obj_update_system(cx, other, session, record.system_id);
        }
    }
    load_selected_character(cx, files, session, &record)
}

fn broadcast(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    let _ = cx.dispatch(ControllerId::BROADCAST, message);
}

/// `PlayerData_LoadSelectedCharacter`: the character's file is read again
/// (a missing or `FLSG` file reads as empty); in single player its
/// `[Time]` and feelings are loaded first; the record is applied to the
/// slot, the client gets the visits, weapon groups, mission log and
/// interface state, and `PlayerLoad` is broadcast with the reader.
pub fn load_selected_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    record: &SavedCharacter,
) -> bool {
    let Some(account) = cx
        .logic
        .players
        .get(session)
        .map(|slot| slot.account_name.clone())
    else {
        return false;
    };
    let path = files
        .accounts
        .account_dir(&account)
        .join(String::from_utf8_lossy(&record.file_name).as_ref());
    let text = std::fs::read(&path)
        .ok()
        .and_then(|bytes| {
            neuron_server::save::read_save(&bytes)
                .text()
                .map(<[u8]>::to_vec)
        })
        .unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    if cx.world.config().single_player {
        load_single_player_state(cx, &mut reader);
    }
    player::apply_saved_character(cx, session, record, session);
    let Some(slot) = cx.logic.players.get(session) else {
        return false;
    };
    let (visits, groups, log, interface) = (
        slot.visits.clone(),
        slot.weapon_groups.clone(),
        slot.mission_log.clone(),
        slot.interface_state,
    );
    client::set_visited_state(cx, session, &visits);
    client::set_weapon_group(cx, session, &groups);
    client::set_mission_log(cx, session, &log);
    client::set_interface_state(cx, session, interface);
    broadcast(
        cx,
        &mut ControllerMessage::PlayerLoad {
            player: session,
            reader: &mut reader,
        },
    );
    true
}

/// `Server_LoadSinglePlayerState`: the first `[Time]` section's first value
/// becomes the game time's base (0 without one), the reader is put back,
/// the faction feelings are replaced by the file's `[Group]` sections, and
/// `LoadGame` is broadcast with the reader.
pub fn load_single_player_state(cx: &mut Cx<'_>, reader: &mut IniReader) {
    let saved = reader.state();
    let mut time = 0.0;
    while reader.read_header() {
        if reader.is_header(b"Time") {
            reader.read_value();
            time = reader.value_float(0);
            break;
        }
    }
    reader.set_state(&saved);
    cx.logic.clock.base = time;
    let reputation = cx.world.reputation_mut();
    reputation.free_feelings();
    reputation.load_feelings(reader, &mut cx.logic.reports);
    broadcast(cx, &mut ControllerMessage::LoadGame { reader });
}
