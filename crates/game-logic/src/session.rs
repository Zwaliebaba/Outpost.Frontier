//! A client's session on the server (work package P3.9): `IServerImpl`'s
//! entry points from the connection to the launch
//! (`Server/IServerImpl.cpp:591-1158`): `OnConnect`, `DisConnect`, `Login`,
//! `CharacterInfoReq`, `CreateNewCharacter`, `CharacterSelect`,
//! `BaseEnter`, `BaseExit`, `LocationEnter`, `LocationExit`,
//! `BaseInfoRequest`, `LocationInfoRequest` and `LaunchComplete`, and the
//! `IClient` senders they need. [`route`] maps the messages of channel 3
//! that reach them, as `RemoteClient`'s `DispatchClientPacket` does
//! (`RemoteClient/RemoteClient_main.cpp:258-690`); the others go to
//! [`crate::receive::deliver`].
//!
//! The player-state requests (work package P3.10b,
//! `Server/IServerImpl.cpp:857-935`, `2071-2330`, `2708`) are here too:
//! the equipment, item and collision-group requests a client makes in a
//! base (`ReqEquipment`, `ReqAddItem`, `ReqRemoveItem`, `ReqModifyItem`,
//! `ReqCollisionGroups`), the state it hands the server to keep
//! (`SetVisitedState`, `SetMissionLog`, `SetInterfaceState`), its
//! questions (`RequestRankLevel`, `RequestBestPath`,
//! `RequestGroupPositions`), `PopUpDialog` and the single-player
//! `ReqDifficultyScale`. [`route_common`] also hands the player trades to
//! [`crate::trade`]. The C++ reads a client's block without its size; a
//! block shorter than what a handler reads is taken only as far as it
//! goes (D881).

use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::costume::Costume as WireCostume;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::protocol::records::{CollisionGroup, EquipRecord};
use game_sim::protocol::to_client::{
    CharSelectVerified, CharacterInfo, CharacterRecord, LoginResponse, MiscObjUpdate, Relation,
    SetReputation,
};
use game_sim::protocol::to_server::{ReqAddItem, ReqModifyItem};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_server::controller::ControllerId;

use crate::character_file::CollisionGroupDesc;
use crate::events::ControllerMessage;
use crate::login::{self, CharacterSet, CreateCharacterInfo, LoginInfo, ServerFiles};
use crate::player::CharacterId;
use crate::server::Cx;

/// The bits of an id that name an object (`& 0x3fffffff`).
const ID_MASK: u32 = 0x3FFF_FFFF;

fn send(
    cx: &mut Cx<'_>,
    client: u32,
    message: impl Into<game_sim::protocol::to_client::ServerMessage>,
) {
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
}

fn broadcast(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    let _ = cx.dispatch(ControllerId::BROADCAST, message);
}

/// `SendServerMiscObjUpdateMotd` (slot +0x1cc, ch2 0x54, mask 0x10).
pub fn misc_obj_update_motd(cx: &mut Cx<'_>, client: u32, text: &[u16]) {
    let message = MiscObjUpdate {
        flags: 0x10,
        text: text.to_vec(),
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `PlayerDB::SetMOTD`: the message is kept and sent to every slot from 1
/// to the player limit whose client id is not 0.
pub fn set_motd(cx: &mut Cx<'_>, text: &[u16]) {
    cx.logic.motd = text.to_vec();
    let max = cx.logic.players.max_players() as u32;
    for session in 1..=max {
        if cx
            .logic
            .players
            .get(session)
            .is_some_and(|s| s.client_id != 0)
        {
            let motd = cx.logic.motd.clone();
            misc_obj_update_motd(cx, session, &motd);
        }
    }
}

/// `IServerImpl::OnConnect`: the new client hears the message of the day.
pub fn on_connect(cx: &mut Cx<'_>, session: u32) {
    let motd = cx.logic.motd.clone();
    misc_obj_update_motd(cx, session, &motd);
}

/// `IServerImpl::DisConnect`: for a logged-in session, a room and a base
/// it is in are left as the client would leave them (the short info
/// request, then the exit), then the session logs out
/// ([`login::logout`]).
pub fn disconnect(cx: &mut Cx<'_>, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let location = cx
        .logic
        .players
        .get(session)
        .map_or(0, |s| s.current_location);
    if location & ID_MASK != 0 {
        location_info_request(cx, location, session, false);
        let location = cx
            .logic
            .players
            .get(session)
            .map_or(0, |s| s.current_location);
        location_exit(cx, location, session);
    }
    let base = cx.logic.players.get(session).map_or(0, |s| s.current_base);
    if base & ID_MASK != 0 {
        base_info_request(cx, base, session, false);
        let base = cx.logic.players.get(session).map_or(0, |s| s.current_base);
        base_exit(cx, base, session);
    }
    login::logout(cx, session);
}

/// `IServerImpl::Login`: on a multiplayer server a slot that has a client
/// already is a second login and is dropped. Otherwise `PlayerDB::login`
/// answers; a 3 broadcasts `PlayerLogin`; the client hears the answer
/// (`FLPACKET_SERVER_LOGINRESPONSE`) and the counters count it.
pub fn login(cx: &mut Cx<'_>, files: &ServerFiles, info: &LoginInfo, session: u32) -> u8 {
    if !cx.world.config().single_player
        && cx
            .logic
            .players
            .get(session)
            .is_some_and(|s| s.client_id != 0)
    {
        return 0;
    }
    let result = login::login(cx, files, info, session);
    if result == 3 {
        broadcast(cx, &mut ControllerMessage::PlayerLogin { player: session });
    }
    send(cx, session, LoginResponse { value: result });
    if result == 3 {
        cx.logic.stats.logins += 1;
    } else {
        cx.logic.stats.failed_logins += 1;
    }
    result
}

/// `RpcServerCharacterRecordFromInfo`'s costume block.
fn wire_costume(costume: &game_sim::data::costume::Costume) -> WireCostume {
    WireCostume {
        parts: costume.part_ids,
        accessories: costume.accessory_ids.clone(),
    }
}

/// `SendServerCharacterInfo` (slot +0x34, ch2 0x03): one record per entry,
/// each item with its hardpoint and terminator and its mission byte.
pub fn send_character_info(cx: &mut Cx<'_>, client: u32, set: &CharacterSet) {
    let strings = cx.world.strings();
    let records = set
        .characters
        .iter()
        .map(|c| CharacterRecord {
            character_id: c.id.as_bytes().to_vec(),
            is_restart: u8::from(c.is_restart),
            is_auto_save: u8::from(c.is_autosave),
            name: c.name.clone(),
            description: c.description.clone(),
            descrip_str_id: c.descrip_str_id,
            timestamp_high: c.timestamp_high,
            timestamp_low: c.timestamp_low,
            ship_archetype: c.ship_archetype,
            money: c.money as u32,
            system_id: c.system_id,
            base_id: c.base_id,
            location_id: c.location_id,
            voice_id: c.voice_id,
            rank: c.rank,
            field_24c: 0,
            hull_status: c.hull_status,
            num_kills: c.num_kills as u32,
            num_mission_failures: c.num_mission_failures as u32,
            num_mission_successes: c.num_mission_successes as u32,
            com_costume: Some(wire_costume(&c.com_costume)),
            base_costume: Some(wire_costume(&c.base_costume)),
            equipment: c
                .equipment
                .iter()
                .map(|item| {
                    let mut hardpoint = strings.get(item.hardpoint).to_vec();
                    hardpoint.push(0);
                    EquipRecord {
                        count: item.count,
                        health: item.health,
                        arch_id: item.arch_id,
                        id: item.id,
                        equipped: item.equipped,
                        field_0f: u8::from(item.mission),
                        hardpoint,
                    }
                })
                .collect(),
            collision_groups: c
                .collision_groups
                .iter()
                .map(|g| CollisionGroup {
                    id: g.id,
                    health: g.health,
                })
                .collect(),
        })
        .collect();
    let message = CharacterInfo {
        records,
        save_failure_count: set.save_failure_count as u32,
    };
    send(cx, client, message);
}

/// `IServerImpl::CharacterInfoReq`: a valid session hears its character
/// list, or an empty one for a full request.
pub fn character_info_req(cx: &mut Cx<'_>, session: u32, full: bool) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let set = if full {
        CharacterSet::default()
    } else {
        login::build_character_set(cx, session).unwrap_or_default()
    };
    send_character_info(cx, session, &set);
}

/// `IServerImpl::CreateNewCharacter`: the list again, the full (empty)
/// form when the character could not be made.
pub fn create_new_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    info: &CreateCharacterInfo,
    session: u32,
) {
    let made = login::create_new_character(cx, files, session, info);
    character_info_req(cx, session, !made);
}

/// `IServerImpl::CharacterSelect`: once the slot has selected the
/// character, every active player hears its vibe (`Vibe::Get` with
/// attitude 0), `SystemEnter` goes to the bus with the slot's system, the
/// client hears the selection with the game time
/// (`FLPACKET_SERVER_CHARSELECTVERIFIED`) and its ship's affiliation.
pub fn character_select(cx: &mut Cx<'_>, files: &ServerFiles, id: &CharacterId, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    if !crate::selection::select_character(cx, files, session, id) {
        return;
    }
    let vibe = cx.logic.players.get(session).map_or(-1, |s| s.rep_vibe);
    let reputation = match cx.world.vibes().0.summary(vibe) {
        Some(s) => SetReputation {
            target: vibe as u32,
            affiliation: s.affiliation,
            display_name: s.display_name.clone(),
            names: (
                s.name1.flatten().unwrap_or_default(),
                s.name2.flatten().unwrap_or_default(),
            ),
            relations: s
                .relations
                .iter()
                .map(|&(group_id, feeling)| Relation { feeling, group_id })
                .collect(),
            attitude: 0.0,
        },
        None => SetReputation {
            target: vibe as u32,
            affiliation: 0,
            names: (
                FmtStr::default().flatten().unwrap_or_default(),
                FmtStr::default().flatten().unwrap_or_default(),
            ),
            ..SetReputation::default()
        },
    };
    let mut after = None;
    while let Some(active) = cx.logic.players.next_active(after) {
        after = Some(active);
        let client = cx.logic.players.get(active).map_or(0, |p| p.client_id);
        send(cx, client, reputation.clone());
    }
    let system = cx.logic.players.get(session).map_or(0, |s| s.system_id);
    broadcast(
        cx,
        &mut ControllerMessage::SystemEnter {
            system,
            from: 0,
            player: session,
            arrival: 0,
            position: neuron_core::math::Vector::ZERO,
        },
    );
    let time = cx.logic.clock.now();
    send(
        cx,
        session,
        CharSelectVerified {
            rep_instance: vibe as u32,
            time,
        },
    );
    let (_, affiliation) = cx.world.vibes().0.affiliation(vibe, false);
    let ship = cx
        .logic
        .players
        .get(session)
        .map_or(0, |s| s.ship_object_id);
    crate::client::misc_obj_update_affiliation(cx, session, ship, affiliation);
}

/// `IServerImpl::BaseEnter`: for a valid session not in a base already and
/// a base the universe has. Entering another base than the one left moves
/// the slot to that base's system (`SystemExit`, then `SystemEnter` from
/// the old system); the slot's base, last base and entered base become
/// `base`, the base's rooms load, `BaseEnter` goes to the bus, and on a
/// multiplayer server the earnings restart and the equipment is
/// snapshotted.
pub fn base_enter(cx: &mut Cx<'_>, base: u32, session: u32) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if slot.entered_base_id & ID_MASK != 0 {
        return;
    }
    let exited = slot.exited_base_id;
    let Some(base_system) = cx.world.data().universe.bases.get(&base).map(|b| b.system) else {
        return;
    };
    if exited != 0 && base != 0 && exited != base {
        let old = cx.logic.players.get(session).map_or(0, |s| s.system_id);
        broadcast(
            cx,
            &mut ControllerMessage::SystemExit {
                system: old,
                destination: base_system,
                player: session,
            },
        );
        if let Some(slot) = cx.logic.players.get_mut(session) {
            slot.system_id = base_system;
        }
        broadcast(
            cx,
            &mut ControllerMessage::SystemEnter {
                system: base_system,
                from: old,
                player: session,
                arrival: 0,
                position: neuron_core::math::Vector::ZERO,
            },
        );
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.current_base = base;
        slot.last_base_id = base;
        slot.entered_base_id = base;
    }
    if let Some(host) = cx.world.data_host() {
        crate::bases::load_base(cx, &host, base);
    }
    broadcast(
        cx,
        &mut ControllerMessage::BaseEnter {
            base,
            player: session,
        },
    );
    if !cx.world.config().single_player {
        crate::player::reset_total_earned(cx, session);
        if let Some(slot) = cx.logic.players.get_mut(session) {
            crate::player::snapshot_equipment(slot);
        }
    }
}

/// `IServerImpl::BaseExit`: for the base the session entered. A room it is
/// in is left first; `BaseExit` goes to the bus, the base unloads, the
/// slot is autosaved, and the base becomes the one left. A refusal sends
/// the client a null packet (`PlayerData_SendClientNullPacket`), which
/// the transport has no form for and drops.
pub fn base_exit(cx: &mut Cx<'_>, base: u32, session: u32) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if slot.entered_base_id & ID_MASK == 0 || base != slot.entered_base_id {
        return;
    }
    let location = slot.current_location;
    if location & ID_MASK != 0 {
        location_info_request(cx, location, session, false);
        let location = cx
            .logic
            .players
            .get(session)
            .map_or(0, |s| s.current_location);
        location_exit(cx, location, session);
    }
    broadcast(
        cx,
        &mut ControllerMessage::BaseExit {
            base,
            player: session,
        },
    );
    crate::bases::unload_base(cx, base);
    let _ = crate::saves::auto_save(cx, session);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.exited_base_id = slot.current_base;
        slot.current_base = 0;
        slot.entered_base_id = 0;
    }
}

/// `IServerImpl::LocationEnter`: for a session in a base and in no room,
/// and a room the base database knows. The room becomes the slot's,
/// `LocationEnter` goes to the bus and the player's character enters it.
pub fn location_enter(cx: &mut Cx<'_>, location: u32, session: u32) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if slot.current_location & ID_MASK != 0 || slot.current_base & ID_MASK == 0 {
        return;
    }
    if crate::bases::find_location(cx, location).is_none() {
        return;
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.current_location = location;
    }
    broadcast(
        cx,
        &mut ControllerMessage::LocationEnter {
            location,
            player: session,
        },
    );
    crate::bases::enter_location(cx, session, location);
}

/// `IServerImpl::LocationExit`: for the room the session is in, inside a
/// base. `LocationExit` goes to the bus, the player's character leaves
/// the room and the slot has no room. A refusal is the null packet.
pub fn location_exit(cx: &mut Cx<'_>, location: u32, session: u32) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if slot.current_base & ID_MASK == 0
        || slot.current_location & ID_MASK == 0
        || slot.current_location != location
    {
        return;
    }
    broadcast(
        cx,
        &mut ControllerMessage::LocationExit {
            location,
            player: session,
        },
    );
    crate::bases::exit_location(cx, session, location);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.current_location = 0;
    }
}

/// `IServerImpl::BaseInfoRequest`: a full request only for the session's
/// own base.
pub fn base_info_request(cx: &mut Cx<'_>, base: u32, session: u32, full: bool) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if full && base != slot.current_base {
        return;
    }
    crate::bases::base_info(cx, session, base, full);
}

/// `IServerImpl::LocationInfoRequest`: a full request only for the
/// session's own room.
pub fn location_info_request(cx: &mut Cx<'_>, location: u32, session: u32, full: bool) {
    let Some(slot) = cx
        .logic
        .players
        .get(session)
        .filter(|_| cx.logic.players.is_valid(session))
    else {
        return;
    };
    if full && location != slot.current_location {
        return;
    }
    crate::bases::location_info(cx, session, location, full);
}

/// `IServerImpl::LaunchComplete(arg, object)`: the object's `slot_70`
/// ([`crate::ship::launch_complete`]).
pub fn launch_complete(cx: &mut Cx<'_>, arg: u32, object: u32) {
    if let Some(h) = cx.world.find(object) {
        let _ = crate::ship::launch_complete(cx, h, arg);
    }
}

/// `SLoginInfo` as `DispatchClientPacket` fills it: the account name cut
/// to 39 characters (`wcsncpy` into 40, the last set to 0) and the create
/// flag.
#[must_use]
pub fn login_info(packet: &game_sim::protocol::to_server::Login) -> LoginInfo {
    let mut account_name: Vec<u16> = packet
        .account_name
        .iter()
        .copied()
        .take_while(|&c| c != 0)
        .collect();
    account_name.truncate(39);
    LoginInfo {
        account_name,
        create: packet.create_flag != 0,
    }
}

/// `SCreateCharacterInfo` as `DispatchClientPacket` fills it: the name to
/// its first NUL (the server's buffer keeps 23 characters, D108), then
/// the faction, base, package and pilot.
#[must_use]
pub fn create_character_info(
    packet: &game_sim::protocol::to_server::CreateNewChar,
) -> CreateCharacterInfo {
    let mut name: Vec<u16> = packet
        .name
        .iter()
        .copied()
        .take_while(|&c| c != 0)
        .collect();
    name.truncate(23);
    CreateCharacterInfo {
        name,
        faction: packet.faction,
        base: packet.base,
        package: packet.package,
        pilot: packet.pilot,
    }
}

/// `DispatchClientPacket` for the channel-3 messages of a session: true
/// when the message reached its `IServerImpl` function here.
pub fn route(cx: &mut Cx<'_>, files: &ServerFiles, session: u32, message: &ToServer) -> bool {
    let ToServer::Client(message) = message else {
        return false;
    };
    match message {
        ClientMessage::Login(p) => {
            let info = login_info(p);
            login(cx, files, &info, session);
        }
        ClientMessage::RequestCharInfo(_) => character_info_req(cx, session, false),
        ClientMessage::SelectCharacter(p) => {
            character_select(cx, files, &CharacterId::new(&p.character_id), session);
        }
        ClientMessage::CreateNewChar(p) => {
            let info = create_character_info(p);
            create_new_character(cx, files, &info, session);
        }
        ClientMessage::EnterBase(p) => base_enter(cx, p.value, session),
        ClientMessage::ExitBase(p) => base_exit(cx, p.value, session),
        ClientMessage::EnterLocation(p) => location_enter(cx, p.value, session),
        ClientMessage::ExitLocation(p) => location_exit(cx, p.value, session),
        ClientMessage::RequestBaseInfo(p) => base_info_request(cx, p.id, session, p.full != 0),
        ClientMessage::RequestLocationInfo(p) => {
            location_info_request(cx, p.id, session, p.full != 0);
        }
        ClientMessage::RequestCreateShip(_) => crate::dock::request_create_ship(cx, session),
        ClientMessage::RequestLaunch(p) => crate::dock::player_launch(cx, p.value, session),
        ClientMessage::LaunchComplete(p) => launch_complete(cx, p.field_00, p.field_04),
        // `case 0x0A` is an empty `break`; `NewCharacterInfoReq` and
        // `ReqCargo` have empty bodies; `SaveGame` returns on a multiplayer
        // server (the single-player save is `crate::saves`).
        ClientMessage::GfRequestShipInfo(_)
        | ClientMessage::RequestNewCharInfo(_)
        | ClientMessage::ReqCargo(_) => {}
        ClientMessage::SaveGame(_) if !cx.world.config().single_player => {}
        ClientMessage::DbgCreateShip(_) => release_mode(cx, 0x26a, "DBGCREATESHIP", "release"),
        ClientMessage::DbgLoadSystem(_) => release_mode(cx, 0x217, "DBGLOADSYSTEM", "release"),
        ClientMessage::DbgDestroyObject(_) => {
            release_mode(cx, 0x1e4, "DBGDESTROYOBJECT", "Final Release");
        }
        ClientMessage::DbgConsole(_) => release_mode(cx, 0x204, "DBGCONSOLE", "release"),
        ClientMessage::DbgFreeSystem(_) => release_mode(cx, 0x227, "DBGFREESYSTEM", "release"),
        ClientMessage::DbgRelocateShip(_) => {
            release_mode(cx, 0x1f5, "DBGRELOCATE_SHIP", "Final Release");
        }
        ClientMessage::DestroyChar(p) => {
            destroy_character(cx, files, &CharacterId::new(&p.character_id), session);
        }
        ClientMessage::ReqShipArch(p) => req_ship_arch(cx, p.value, session),
        ClientMessage::ReqEquipment(p) => {
            let list = equip_desc_list(cx, &p.records);
            req_equipment(cx, &list, session);
        }
        ClientMessage::ReqAddItem(p) => req_add_item(cx, p, session),
        ClientMessage::ReqRemoveItem(p) => {
            req_remove_item(cx, low_word(p.equip_id), p.count, session);
        }
        ClientMessage::ReqModifyItem(p) => req_modify_item(cx, p, session),
        ClientMessage::ReqCollisionGroups(p) => req_collision_groups(cx, &p.groups, session),
        ClientMessage::ReqDifficultyScale(p) => req_difficulty_scale(cx, p.value, session),
        ClientMessage::TradeResponse(p) => crate::trade::trade_response(cx, session, p),
        ClientMessage::ReqHullStatus(p) => req_hull_status(cx, p.value, session),
        ClientMessage::ReqSetCash(p) => {
            if in_entered_base(cx, session) {
                crate::player::set_cash(cx, session, p.value);
            }
        }
        ClientMessage::ReqChangeCash(p) => {
            if in_entered_base(cx, session) {
                crate::player::adjust_cash(cx, session, p.value, false);
            }
        }
        ClientMessage::RequestEvent(p) => crate::dock::request_event(
            cx,
            i32::from(p.event_type),
            p.arg1,
            p.arg2,
            p.arg3,
            i32::from(p.field_10 as i8) as u32,
        ),
        ClientMessage::RequestCancel(p) => crate::dock::request_cancel(
            cx,
            i32::from(p.event_type),
            p.arg1,
            p.arg2,
            i32::from(p.field_0c as i8) as u32,
        ),
        ClientMessage::GfSelectObject(p) => gf_obj_select(cx, p.value, session),
        ClientMessage::GfGoodBuy(p) => broadcast(
            cx,
            &mut ControllerMessage::GfGoodBuy {
                info: [p.field_00, p.field_04, p.field_08, p.field_0c],
                player: session,
            },
        ),
        ClientMessage::GfGoodSell(p) => broadcast(
            cx,
            &mut ControllerMessage::GfGoodSell {
                info: [p.field_00, p.field_04, p.field_08],
                player: session,
            },
        ),
        ClientMessage::GfGoodVaporized(p) => broadcast(
            cx,
            &mut ControllerMessage::GfGoodVaporized {
                info: [p.field_00, p.field_04, p.field_08],
                player: session,
            },
        ),
        ClientMessage::MissionResponse(p) => broadcast(
            cx,
            &mut ControllerMessage::MissionResponse {
                offer: p.field_00,
                kind: p.mission_id,
                accepted: p.accepted != 0,
                player: session,
            },
        ),
        ClientMessage::Hail(p) => {
            if cx.world.find(p.field_00).is_some() {
                broadcast(
                    cx,
                    &mut ControllerMessage::Hail {
                        object: p.field_00,
                        arg1: p.field_04,
                        arg2: p.field_08,
                    },
                );
            }
        }
        ClientMessage::CargoScan(p) => broadcast(
            cx,
            &mut ControllerMessage::CargoScanned {
                scanner: p.field_00,
                target: p.field_04,
            },
        ),
        ClientMessage::CommComplete(p) => {
            comm_complete(cx, p.field_00, p.field_04, p.field_08, p.field_0c);
        }
        ClientMessage::InterfaceItemUsed(p) => broadcast(
            cx,
            &mut ControllerMessage::InterfaceItemUsed {
                player: session,
                item: p.value,
            },
        ),
        ClientMessage::AbortMission(p) => {
            broadcast(
                cx,
                &mut ControllerMessage::AbortMission {
                    player: session,
                    mission: p.value,
                },
            );
            cx.logic.stats.aborted_missions += 1;
        }
        ClientMessage::RtcDone(p) => broadcast(
            cx,
            &mut ControllerMessage::RtcDone {
                player: session,
                rtc: p.value,
            },
        ),
        _ => return false,
    }
    true
}

/// `DispatchClientPacket`'s channel-1 messages that reach `IServerImpl`'s
/// player slots here: `SetWeaponGroup`, `RequestPlayerStats`, the
/// player-state requests and the player trades.
pub fn route_common(cx: &mut Cx<'_>, session: u32, message: &ToServer) -> bool {
    let ToServer::Common(message) = message else {
        return false;
    };
    match message {
        CommonMessage::SetWeaponGroup(p) => set_weapon_group(cx, session, &p.data),
        CommonMessage::PlayerTrade(p) => crate::trade::deliver_player_trade(cx, session, p),
        CommonMessage::SetVisitedState(p) => set_visited_state(cx, session, &p.data),
        CommonMessage::RequestBestPath(p) => request_best_path(cx, session, &p.data),
        CommonMessage::RequestGroupPositions(_) => request_group_positions(cx, session),
        CommonMessage::SetMissionLog(p) => set_mission_log(cx, session, &p.data),
        CommonMessage::RequestRankLevel(_) => request_rank_level(cx, session),
        CommonMessage::PopUpDialog(p) => pop_up_dialog(cx, session, p.field_10),
        CommonMessage::SetInterfaceState(p) => set_interface_state(cx, session, &p.data),
        CommonMessage::RequestPlayerStats(_) => {
            if logged_in(cx, session) {
                broadcast(
                    cx,
                    &mut ControllerMessage::RequestPlayerStats { player: session },
                );
            }
        }
        _ => return false,
    }
    true
}

/// `DispatchClientPacket`'s answer to a debug packet: an error report and
/// nothing else.
fn release_mode(cx: &mut Cx<'_>, line: u32, packet: &str, mode: &str) {
    cx.logic.reports.push(
        "Network\\OutpostServerPacketHandler.cpp",
        line,
        format!("*** ERROR: FLPACKET_CLIENT_{packet} encountered in {mode} mode"),
    );
}

/// `PlayerDB_GetPlayer`'s test: a session from 1 to the limit whose slot
/// has a client.
fn logged_in(cx: &Cx<'_>, session: u32) -> bool {
    cx.logic.players.is_valid(session)
}

/// The `Req*` handlers' gate: single player, or a base entered
/// (`entered_base_id != 0`). The C++ indexes the slot unchecked; a session
/// without one is refused.
fn in_entered_base(cx: &Cx<'_>, session: u32) -> bool {
    cx.logic
        .players
        .get(session)
        .is_some_and(|s| cx.world.config().single_player || s.entered_base_id != 0)
}

/// `IServerImpl::DestroyCharacter`: for a valid session, the character
/// goes (`PlayerData_DestroyCharacter`) and the list is sent again.
pub fn destroy_character(cx: &mut Cx<'_>, files: &ServerFiles, id: &CharacterId, session: u32) {
    if !cx.logic.players.is_valid(session) {
        return;
    }
    let _ = login::destroy_character(cx, files, session, id);
    character_info_req(cx, session, false);
}

/// `IServerImpl::ReqShipArch`: in a base entered, the slot's ship.
pub fn req_ship_arch(cx: &mut Cx<'_>, arch: u32, session: u32) {
    if in_entered_base(cx, session) {
        crate::player::set_ship_arch(cx, session, arch);
    }
}

/// `IServerImpl::ReqHullStatus`: for a valid session in a base entered and
/// docked, the hull's health, a negative one taken as 0, told back to the
/// client.
pub fn req_hull_status(cx: &mut Cx<'_>, health: f32, session: u32) {
    if !cx.logic.players.is_valid(session) || !in_entered_base(cx, session) {
        return;
    }
    if cx
        .logic
        .players
        .get(session)
        .is_none_or(|s| s.current_base & ID_MASK == 0)
    {
        return;
    }
    let health = if health < 0.0 { 0.0 } else { health };
    crate::player::set_relative_health(cx, session, health, true);
}

/// `IServerImpl::GFObjSelect`: for a session in a base the universe has,
/// `GFObjSelect` goes to the bus.
pub fn gf_obj_select(cx: &mut Cx<'_>, object: u32, session: u32) {
    let Some(base) = cx.logic.players.get(session).map(|s| s.current_base) else {
        return;
    };
    if base & ID_MASK == 0 || !cx.world.data().universe.bases.contains_key(&base) {
        return;
    }
    broadcast(
        cx,
        &mut ControllerMessage::GfObjSelect {
            object,
            player: session,
        },
    );
}

/// `IServerImpl::CommComplete(arg0, arg1, arg2, result)`: the system of
/// the object `arg1`, else of `arg0` (0 when neither is in one), then
/// `CommComplete` with `{arg1, system, arg0, arg2, result}`.
pub fn comm_complete(cx: &mut Cx<'_>, arg0: u32, arg1: u32, arg2: u32, result: u32) {
    let system_of = |cx: &Cx<'_>, id: u32| {
        cx.world
            .find(id)
            .and_then(|h| cx.world.get(h))
            .and_then(|o| o.simple.system)
    };
    let system = system_of(cx, arg1)
        .or_else(|| system_of(cx, arg0))
        .unwrap_or(0);
    broadcast(
        cx,
        &mut ControllerMessage::CommComplete {
            listener: arg1,
            system,
            speaker: arg0,
            line: arg2,
            result: crate::events::CommResult::from_u32(result),
        },
    );
}

/// `IServerImpl::SetWeaponGroup`: a valid session's weapon groups become
/// the text up to its first NUL (`basic_string::assign(const char*)`).
pub fn set_weapon_group(cx: &mut Cx<'_>, session: u32, data: &[u8]) {
    if !logged_in(cx, session) {
        return;
    }
    let end = data.iter().position(|&b| b == 0).unwrap_or(data.len());
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.weapon_groups = data[..end].to_vec();
    }
}

// ---------------------------------------------------------------------------
// The player-state requests (P3.10b).

/// The `EquipDescList` `DispatchClientPacket` builds of a `REQEQUIPMENT`
/// (`RemoteClient_main.cpp:419-452`): each record a new `EquipDesc` with
/// its archetype, count, health, id and mounted byte, on the hardpoint it
/// names (`StringAlloc`) or, without one, in the cargo bay; the record's
/// last byte is not read.
#[must_use]
pub fn equip_desc_list(cx: &mut Cx<'_>, records: &[EquipRecord]) -> Vec<EquipDesc> {
    records
        .iter()
        .map(|record| {
            let mut desc = EquipDesc::new(cx.world.strings());
            desc.arch_id = record.arch_id;
            desc.count = record.count;
            desc.health = record.health;
            desc.id = record.id;
            desc.equipped = record.equipped;
            if record.hardpoint.is_empty() {
                desc.make_internal(cx.world.strings());
            } else {
                desc.hardpoint = cx.world.strings_mut().alloc(&record.hardpoint, false);
            }
            desc
        })
        .collect()
}

/// `IServerImpl::ReqEquipment` (`IServerImpl.cpp:866`): in a base entered
/// (or in single player), the list replaces the slot's equipment
/// ([`crate::player::load_equipment`]).
pub fn req_equipment(cx: &mut Cx<'_>, list: &[EquipDesc], session: u32) {
    if in_entered_base(cx, session) {
        crate::player::load_equipment(cx, session, list);
    }
}

/// `IServerImpl::ReqAddItem` (`IServerImpl.cpp:903`): in a base entered,
/// the item joins the slot's equipment on the hardpoint the block names
/// (`StringAlloc`), combined where it may and told back to the client
/// ([`crate::player::add_equipment`]).
pub fn req_add_item(cx: &mut Cx<'_>, p: &ReqAddItem, session: u32) {
    if !in_entered_base(cx, session) {
        return;
    }
    let hardpoint = cx.world.strings_mut().alloc(&p.hardpoint, false);
    crate::player::add_equipment(
        cx, session, p.arch_id, hardpoint, p.count, p.health, p.mounted, false, true,
    );
}

/// `IServerImpl::ReqRemoveItem` (`IServerImpl.cpp:918`): in a base
/// entered, the count leaves the item; the client, which asked, is not
/// told ([`crate::player::remove_equipment`]).
pub fn req_remove_item(cx: &mut Cx<'_>, id: u16, count: i32, session: u32) {
    if in_entered_base(cx, session) {
        crate::player::remove_equipment(cx, session, id, count, false);
    }
}

/// `IServerImpl::ReqModifyItem` (`IServerImpl.cpp:929`): in a base
/// entered, the item takes the hardpoint, count, health and mounted byte
/// ([`crate::player::modify_equipment`]); the id is the packet's low word.
pub fn req_modify_item(cx: &mut Cx<'_>, p: &ReqModifyItem, session: u32) {
    if !in_entered_base(cx, session) {
        return;
    }
    let hardpoint = cx.world.strings_mut().alloc(&p.hardpoint, false);
    crate::player::modify_equipment(
        cx,
        session,
        low_word(p.equip_id),
        hardpoint,
        p.count,
        p.health,
        p.mounted,
    );
}

/// The `unsigned short` of a packet's dword.
fn low_word(value: u32) -> u16 {
    let [a, b, ..] = value.to_le_bytes();
    u16::from_le_bytes([a, b])
}

/// `IServerImpl::ReqCollisionGroups` (`IServerImpl.cpp:857`): in a base
/// entered, the groups replace the slot's and go back to the client
/// ([`crate::player::set_collision_groups`]).
pub fn req_collision_groups(cx: &mut Cx<'_>, groups: &[CollisionGroup], session: u32) {
    if !in_entered_base(cx, session) {
        return;
    }
    let groups: Vec<CollisionGroupDesc> = groups
        .iter()
        .map(|g| CollisionGroupDesc {
            id: g.id,
            health: g.health,
        })
        .collect();
    crate::player::set_collision_groups(cx, session, &groups);
}

/// `IServerImpl::ReqDifficultyScale` (`IServerImpl.cpp:2708`): in single
/// player, a valid session's difficulty becomes the scale held to 0 ..= 2
/// (a NaN is 0).
pub fn req_difficulty_scale(cx: &mut Cx<'_>, scale: f32, session: u32) {
    if !cx.world.config().single_player || !logged_in(cx, session) {
        return;
    }
    let scale = if scale > 0.0 { scale } else { 0.0 };
    let scale = if scale < 2.0 { scale } else { 2.0 };
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.difficulty_scale = scale;
    }
}

/// The little-endian dword at `at` of a client's block, `None` past its
/// end.
fn dword(data: &[u8], at: usize) -> Option<u32> {
    let bytes = data.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// `IServerImpl::SetVisitedState` (`IServerImpl.cpp:2071`): for a valid
/// session, each of the block's records (a signed count, then an id and a
/// flag byte each) is ORed into the slot's visits or added to them.
pub fn set_visited_state(cx: &mut Cx<'_>, session: u32, data: &[u8]) {
    if !logged_in(cx, session) {
        return;
    }
    let Some(count) = dword(data, 0) else {
        return;
    };
    let count = usize::try_from(i32::from_ne_bytes(count.to_ne_bytes())).unwrap_or(0);
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return;
    };
    let (records, _) = data[4..].as_chunks::<5>();
    for &[a, b, c, d, flags] in records.iter().take(count) {
        *slot.visits.entry(u32::from_le_bytes([a, b, c, d])).or_insert(0) |= flags;
    }
}

/// `IServerImpl::SetMissionLog` (`IServerImpl.cpp:2237`): for a valid
/// session, operation 0 empties the slot's mission log; operation 1 puts
/// each of the block's format strings (a signed count at +4, the strings
/// from +8) at the front of it, so the last comes first; any other does
/// nothing.
pub fn set_mission_log(cx: &mut Cx<'_>, session: u32, data: &[u8]) {
    if !logged_in(cx, session) {
        return;
    }
    let Some(op) = dword(data, 0) else {
        return;
    };
    match op {
        0 => {
            if let Some(slot) = cx.logic.players.get_mut(session) {
                slot.mission_log.clear();
            }
        }
        1 => {
            let Some(count) = dword(data, 4) else {
                return;
            };
            let mut count = i32::from_ne_bytes(count.to_ne_bytes());
            let mut at = 8;
            let mut entries = Vec::new();
            while count > 0 {
                let Ok((entry, used)) = FmtStr::unflatten(&data[at..]) else {
                    break;
                };
                entries.push(entry);
                at += used;
                count -= 1;
            }
            if let Some(slot) = cx.logic.players.get_mut(session) {
                for entry in entries {
                    slot.mission_log.insert(0, entry);
                }
            }
        }
        _ => {}
    }
}

/// `IServerImpl::SetInterfaceState` (`IServerImpl.cpp:2283`): for a valid
/// session, the block's first dword is the slot's interface state.
pub fn set_interface_state(cx: &mut Cx<'_>, session: u32, data: &[u8]) {
    if !logged_in(cx, session) {
        return;
    }
    let Some(state) = dword(data, 0) else {
        return;
    };
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.interface_state = state;
    }
}

/// `IServerImpl::RequestRankLevel` (`IServerImpl.cpp:2290`): a valid
/// session's client hears its rank in single player, 0xFFFFFFFF on a
/// multiplayer server (`SendCommonRequestRankLevel`, four bytes). The
/// request's block is not read.
pub fn request_rank_level(cx: &mut Cx<'_>, session: u32) {
    if !logged_in(cx, session) {
        return;
    }
    let rank = if cx.world.config().single_player {
        cx.logic.players.get(session).map_or(0, |s| s.rank_level)
    } else {
        0xFFFF_FFFF
    };
    crate::client::request_rank_level(cx, session, rank.to_le_bytes().to_vec());
}

/// `IServerImpl::RequestBestPath` (`IServerImpl.cpp:2100`): for a valid
/// session, `RequestBestPath` goes to the bus with the session and the
/// block's dwords at +0x00, +0x1c, +0x0c, +0x10, +0x14, +0x20, +0x24,
/// +0x28 and +0x30.
pub fn request_best_path(cx: &mut Cx<'_>, session: u32, data: &[u8]) {
    if !logged_in(cx, session) {
        return;
    }
    let mut words = [0u32; 9];
    for (word, at) in words
        .iter_mut()
        .zip([0x00, 0x1c, 0x0c, 0x10, 0x14, 0x20, 0x24, 0x28, 0x30])
    {
        let Some(value) = dword(data, at) else {
            return;
        };
        *word = value;
    }
    broadcast(
        cx,
        &mut ControllerMessage::RequestBestPath {
            player: session,
            words,
        },
    );
}

/// `IServerImpl::RequestGroupPositions` (`IServerImpl.cpp:2171`): a valid
/// session's client hears where its group's other members are
/// (`SendCommonRequestGroupPositions`): their count, then per member its
/// position, base, system and vibe (0x18 bytes). A member with a ship
/// gives the ship's position in its system (zeros when the system has no
/// such ship) and base 0; one without gives its base and, where the C++
/// leaves the position as allocated, zeros (D882). Without a group the
/// block is the count 0. The request's block is not read.
pub fn request_group_positions(cx: &mut Cx<'_>, session: u32) {
    if !logged_in(cx, session) {
        return;
    }
    let Some(group) = cx.logic.players.get(session).and_then(|s| s.group) else {
        crate::client::request_group_positions(cx, session, vec![0; 4]);
        return;
    };
    let members: Vec<u32> = cx
        .logic
        .groups
        .get(group)
        .map(|g| g.members.iter().copied().collect())
        .unwrap_or_default();
    let others = members.len().saturating_sub(1);
    let size = others * 0x18 + 4;
    let mut data = Vec::with_capacity(size);
    data.extend_from_slice(&u32::try_from(others).unwrap_or(u32::MAX).to_le_bytes());
    for member in members {
        if member == session {
            continue;
        }
        let (system, vibe, ship, base) = cx.logic.players.get(member).map_or((0, 0, 0, 0), |m| {
            (m.system_id, m.rep_vibe, m.ship_object_id, m.current_base)
        });
        let (position, base) = if ship != 0 {
            let at = crate::transfer::lookup_system(cx, system)
                .and_then(|system| crate::replication::find_in_system(cx.world, system, ship))
                .and_then(|h| cx.world.get(h))
                .map_or([0.0; 3], |o| {
                    let p = o.engine.position;
                    [p.x, p.y, p.z]
                });
            (at, 0)
        } else {
            ([0.0; 3], base)
        };
        for value in position {
            data.extend_from_slice(&value.to_le_bytes());
        }
        data.extend_from_slice(&base.to_le_bytes());
        data.extend_from_slice(&system.to_le_bytes());
        data.extend_from_slice(&vibe.to_le_bytes());
    }
    // The block is as long as the count says, as the C++ allocates it.
    data.resize(size, 0);
    crate::client::request_group_positions(cx, session, data);
}

/// `IServerImpl::PopUpDialog(session, button)` (`IServerImpl.cpp:2138`):
/// `PopUpDialog` goes to the bus with the session and the packet's last
/// dword, unchecked.
pub fn pop_up_dialog(cx: &mut Cx<'_>, session: u32, button: u32) {
    broadcast(
        cx,
        &mut ControllerMessage::PopUpDialog {
            player: session,
            button,
        },
    );
}
