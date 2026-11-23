//! The GameSupport controller (`Content/GameSupport.cpp`, TU33; record 0008
//! point 2) and Content's game time.
//!
//! - **Constructor** (`FGameSupport::FGameSupport`, `:450-458`): a 1 s
//!   heartbeat (one `rand()` for its phase), the message router (its timer
//!   id from Content's counter), then `LoadContentGameTables`.
//! - **Destructor** (`:461-468`): `UnloadContentGameTables`, then the router
//!   is deleted.
//! - **Heartbeat** (`HandleMessage`'s `Heartbeat` case, `:594-599`): the
//!   elapsed time is added to the game time (`GGameSupportElapsed`, a
//!   `double`), then to every player state's play time
//!   (`UpdateAllContentPlayerTimes`).
//! - **Game time** (`GetContentGameTime`, `GetContentTimeDistance`,
//!   `:782-797`): what the router's delivery times and the comm system's
//!   intervals read.
//! - **The run time** (`HandleMessage`, `:544-765`; P3.4d): the player
//!   states ([`super::players`]: made on `PlayerLoad` from `[mPlayer]`,
//!   written on `PlayerSave`, dropped on `PlayerLogout`) with their ship,
//!   system, base and location records and the rank; the player's ship
//!   object on `SpaceEnter` (`OnPlayerLaunch`); the lane ships of the
//!   system records on `TradelaneEnter` and `TradelaneExit`; `LoadGame`
//!   (the base records, the gate locks, the system records and the system
//!   paths) and `SaveGame` (the gate locks).
//!
//! A base's bar characters on `BaseEnter` and `BaseExit`
//! (`FContentBase::OnPlayerEnter`, `OnPlayerExit`) are BaseSupport's run
//! time (P3.6e).

use std::io::Write;
use std::sync::Arc;

use game_sim::ai::state_graph::GraphType;
use neuron_core::ini::IniReader;
use neuron_server::controller::ControllerId;

use super::players::{self, PlayerShip, PlayerState};
use super::{ContentTables, Instance, bases, data_source, router::Router, systems};
use crate::api;
use crate::api::ai::{BaseOp, CallbackArg, Op, SetPersonalityParams};
use crate::events::ControllerMessage;
use crate::server::Cx;

/// GameSupport's heartbeat period in seconds (`:453`).
pub const HEARTBEAT: f32 = 1.0;

/// The game time's "no time" value (`-1.0`, compared as its bits).
pub const NO_TIME: f64 = -1.0;

/// `CreateID("fighter")` (`ContentStaticIds.h:29`): a fighter's class.
fn fighter_class() -> u32 {
    neuron_core::id::create_id(b"fighter")
}

/// `CreateID("freighter")` (`ContentStaticIds.h:30`).
fn freighter_class() -> u32 {
    neuron_core::id::create_id(b"freighter")
}

/// `FGameSupport::FGameSupport(ControllerId)`.
pub fn construct(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, HEARTBEAT);
    let router = Router::new(&mut cx.logic.content.state.ids);
    // A second GameSupport replaces the router; the C++ leaks the first.
    cx.logic.content.state.router = Some(router);
    let tables = ContentTables::load_from_world(cx);
    cx.logic.content.tables = Arc::new(tables);
    Instance::GameSupport
}

/// `FGameSupport::~FGameSupport`: the tables unloaded, the router deleted
/// (`GMessageRouter` cleared by the router's base destructor).
pub fn destruct(cx: &mut Cx<'_>, _id: ControllerId) {
    cx.logic.content.tables = Arc::new(ContentTables::default());
    // `UnloadContentGameTables` also empties the base records
    // (`UnloadContentBases`) and the system records (`ClearSystemInfos`).
    cx.logic.content.state.bases.clear();
    cx.logic.content.state.systems.clear();
    cx.logic.content.state.router = None;
}

/// `FGameSupport::HandleMessage` (`:544-765`).
///
/// Where the C++ dereferences a player state or system record it did not
/// test (`PlayerWorthChanged`, `BaseEnter`, `PlayerSave`, `LocationEnter`,
/// `LocationExit`, `SpaceExit`, `SystemExit`, `SystemEnter`,
/// `TradelaneEnter`, `TradelaneExit`, `SpaceEnter`), the port does nothing
/// without one (D801).
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    match message {
        ControllerMessage::PlayerWorthChanged { client } => {
            if let Some(state) = cx.logic.content.state.players.get(client).cloned() {
                players::recalculate_rank(cx, &state);
            }
        }
        ControllerMessage::BaseEnter { base, player } => {
            let (base, player) = (*base, *player);
            on_base_change(cx, player, base);
            if cx.logic.content.state.bases.contains_key(&base) {
                load_base(cx, base);
                // `Base->OnPlayerEnter(PlayerId)`: the bar characters (P3.6e).
            }
        }
        ControllerMessage::BaseExit { player, .. } => {
            let player = *player;
            if cx.logic.content.state.players.contains_key(&player) {
                // `Base->OnPlayerExit(PlayerId)`: the bar characters (P3.6e).
                on_base_change(cx, player, 0);
            }
        }
        ControllerMessage::PlayerSave { client, file } => {
            let client = *client;
            save_player(cx, client, &mut **file);
        }
        ControllerMessage::PlayerLoad { player, reader } => {
            let player = *player;
            load_player(cx, player, reader);
        }
        ControllerMessage::Heartbeat { elapsed } => heartbeat(cx, *elapsed),
        ControllerMessage::LocationEnter { location, player } => {
            if let Some(state) = cx.logic.content.state.players.get_mut(player) {
                state.location = *location;
            }
        }
        ControllerMessage::LocationExit { player, .. } => {
            if let Some(state) = cx.logic.content.state.players.get_mut(player) {
                state.location = 0;
            }
        }
        ControllerMessage::SpaceExit { player, .. } => {
            if let Some(state) = cx.logic.content.state.players.get_mut(player) {
                state.ship = None;
            }
        }
        ControllerMessage::ObjectDestroyed(event) => {
            let states = &mut cx.logic.content.state.players;
            if let Some(player) = players::find_by_ship(states, event.id)
                && let Some(state) = states.get_mut(&player)
            {
                state.ship = None;
                state.ship_destroyed = true;
            }
        }
        ControllerMessage::SystemExit { player, .. } => {
            let player = *player;
            on_system_change(cx, player, 0);
        }
        ControllerMessage::PlayerLogout { player } => {
            cx.logic.content.state.players.remove(player);
        }
        ControllerMessage::SystemEnter { system, player, .. } => {
            let (system, player) = (*system, *player);
            on_system_change(cx, player, system);
        }
        ControllerMessage::LoadGame { reader } => load_game(cx, reader),
        ControllerMessage::SaveGame { file } => {
            // `SaveContentBases` writes nothing; `VisitSystemInfos` walks
            // the records and does nothing.
            let locks = cx.logic.content.state.gate_locks.clone();
            let _ = locks.save(&mut **file);
        }
        ControllerMessage::TradelaneEnter {
            ship,
            entrance,
            exit,
            system,
        } => tradelane_enter(cx, *ship, *entrance, *exit, *system),
        ControllerMessage::TradelaneExit {
            ship,
            entrance,
            system,
            ..
        } => tradelane_exit(cx, *ship, *entrance, *system),
        ControllerMessage::Msg0x25 {
            ship,
            death_sequence,
            ..
        } => target_changed(cx, *ship, *death_sequence),
        ControllerMessage::SpaceEnter { player, .. } => {
            let player = *player;
            player_launch(cx, player);
        }
        _ => {}
    }
}

/// `FContentBase::Load(nullptr)` of the base's record: on the first load,
/// its sections of `missions\mBases.ini` (P3.4c, [`bases::ContentBase::load`]).
fn load_base(cx: &mut Cx<'_>, base: u32) {
    let Some(mut record) = cx.logic.content.state.bases.remove(&base) else {
        return;
    };
    let take = super::NoDispatch::begin(&cx.logic.content);
    let source = data_source(cx);
    record.load(cx, None, source.as_ref());
    take.end(&cx.logic.content);
    cx.logic.content.state.bases.insert(base, record);
}

/// `FContentPlayerState::OnBaseChange(BaseId)` (`PlayerState.cpp:213-220`):
/// the base record (`None` when there is none, 0 included), and a base
/// whose low 30 bits are not 0 joins the visited bases once.
fn on_base_change(cx: &mut Cx<'_>, player: u32, base: u32) {
    let record = cx
        .logic
        .content
        .state
        .bases
        .contains_key(&base)
        .then_some(base);
    let Some(state) = cx.logic.content.state.players.get_mut(&player) else {
        return;
    };
    state.base = record;
    if base & 0x3fff_ffff != 0 && !state.stats.bases_visited.contains(&base) {
        state.stats.bases_visited.push(base);
    }
}

/// `FContentPlayerState::OnSystemChange(SystemId)` (`PlayerState.cpp:194-210`):
/// the system record (`None` for 0 or an unknown system), handed to the
/// ship object; then a system whose low 30 bits are not 0 other than
/// `RELAYSEVEN_system` (looked up again each time) joins the visited
/// systems once.
fn on_system_change(cx: &mut Cx<'_>, player: u32, system: u32) {
    let record = cx
        .logic
        .content
        .state
        .systems
        .contains_key(&system)
        .then_some(system);
    let Some(state) = cx.logic.content.state.players.get_mut(&player) else {
        return;
    };
    state.system = record;
    if let Some(ship) = state.ship.as_mut() {
        ship.system = record;
    }
    if system & 0x3fff_ffff == 0 {
        return;
    }
    if let Ok(id) = api::misc::system_id(cx, players::RELAY_SEVEN_SYSTEM) {
        cx.logic.content.state.relay_seven_system = id;
    }
    if system == cx.logic.content.state.relay_seven_system {
        return;
    }
    if let Some(state) = cx.logic.content.state.players.get_mut(&player)
        && !state.stats.systems_visited.contains(&system)
    {
        state.stats.systems_visited.push(system);
    }
}

/// `PlayerSave`: the player's `[mPlayer]` ([`PlayerState::save`]).
fn save_player(cx: &mut Cx<'_>, player: u32, out: &mut dyn Write) {
    let Some(state) = cx.logic.content.state.players.get(&player).cloned() else {
        return;
    };
    let locks = cx.logic.content.state.gate_locks.clone();
    let _ = state.save(cx, &locks, out);
}

/// `OnPlayerLoad` (`:470-476`): the player's state replaced by a new one
/// (`ReplaceContentPlayerState`), the reader reset, then `[mPlayer]` read
/// ([`players::load_player_state`]).
fn load_player(cx: &mut Cx<'_>, player: u32, reader: &mut IniReader) {
    cx.logic.content.state.players.remove(&player);
    let mut state = PlayerState::new(cx, player);
    reader.reset();
    players::load_player_state(cx, &mut state, reader);
    cx.logic.content.state.players.insert(player, state);
}

/// The `LoadGame` case (`:657-668`): the base records again
/// ([`bases::load_dyn_base_info`]), the gate locks, the system records
/// ([`systems::load_dyn_sys_info`]), each over the reader from its start,
/// then the system paths (`InitSystemPathTables`).
///
/// The system paths are read back from `Universe\systems_shortest_path.ini`
/// when it exists. When it does not, the C++ builds them and writes the
/// three files next to its working directory; the port builds them and
/// writes nothing (D804).
pub fn load_game(cx: &mut Cx<'_>, reader: &mut IniReader) {
    let source = data_source(cx);
    let mut bases = std::mem::take(&mut cx.logic.content.state.bases);
    bases::load_dyn_base_info(cx, &mut bases, source.as_ref(), reader);
    cx.logic.content.state.bases = bases;
    reader.reset();
    cx.logic.content.state.gate_locks.load(reader);
    reader.reset();
    systems::load_dyn_sys_info(cx, reader);
    reader.reset();
    let mut paths = std::mem::take(&mut cx.logic.content.state.system_paths);
    if !paths.init(cx, source.as_ref()) {
        let systems: Vec<u32> = cx.logic.content.state.systems.keys().copied().collect();
        let locks = cx.logic.content.state.gate_locks.clone();
        paths.build(cx, &systems, &|object| locks.is_locked(0, object));
    }
    cx.logic.content.state.system_paths = paths;
}

/// The `TradelaneEnter` case (`:680-720`): a ship of the system record
/// (a player's ship; the NPC ships are P3.6c's) that enters the lane of
/// the entry ring travels towards the last ring when the entry comes
/// before the target ring (an unsigned compare of their indices, -1 for a
/// ring not on the lane), else towards the first; its lane pair is set and
/// it joins the lane's ships in that direction. Nothing for an entry ring
/// without a lane, whose null lane the C++ dereferences (D801).
fn tradelane_enter(cx: &mut Cx<'_>, ship: u32, entry: u32, target: u32, system: u32) {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    let Some(player) = systems::find_player_ship(cx, record, ship) else {
        return;
    };
    let Some(rings) = record.lane_rings_of_ring(entry) else {
        return;
    };
    let mut entry_index = u32::MAX;
    let mut target_index = u32::MAX;
    for (index, ring) in rings.iter().enumerate() {
        if ring.object == entry {
            entry_index = index as u32;
            if target_index != u32::MAX {
                break;
            }
        } else if ring.object == target {
            target_index = index as u32;
            if entry_index != u32::MAX {
                break;
            }
        }
    }
    let (end, direction) = if entry_index < target_index {
        (rings[rings.len() - 1].object, 1)
    } else {
        (rings[0].object, -1)
    };
    if let Some(s) = cx
        .logic
        .content
        .state
        .players
        .get_mut(&player)
        .and_then(|p| p.ship.as_mut())
    {
        s.lane_entry = entry;
        s.lane_end = end;
    }
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        record.add_trade_lane_ship(entry, direction, ship);
    }
}

/// The `TradelaneExit` case (`:722-734`): the ship's lane pair cleared and
/// the ship removed from the ring's lane.
fn tradelane_exit(cx: &mut Cx<'_>, ship: u32, ring: u32, system: u32) {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    let Some(player) = systems::find_player_ship(cx, record, ship) else {
        return;
    };
    if let Some(s) = cx
        .logic
        .content
        .state
        .players
        .get_mut(&player)
        .and_then(|p| p.ship.as_mut())
    {
        s.lane_entry = 0;
        s.lane_end = 0;
    }
    let now = game_time(cx);
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        record.remove_trade_lane_ship(ship, ring, now);
    }
}

/// The `Msg0x25` case (`:736-744`) and `FContentPlayerShip::OnTargetChanged`
/// (`PlayerState.cpp:132-153`): the player's ship remembers the target; a
/// jump hole (type bit `0x80000`) and its exit join the visited holes once
/// each.
fn target_changed(cx: &mut Cx<'_>, ship: u32, target: u32) {
    let Some(player) = players::find_by_ship(&cx.logic.content.state.players, ship) else {
        return;
    };
    if let Some(s) = cx
        .logic
        .content
        .state
        .players
        .get_mut(&player)
        .and_then(|p| p.ship.as_mut())
    {
        s.target = target;
    }
    let kind = api::space_obj::object_type(cx, target).unwrap_or(0);
    if kind & 0x8_0000 == 0 {
        return;
    }
    let (_, exit) = api::space_obj::jump_target(cx, target).unwrap_or((0, 0));
    if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
        let holes = &mut state.stats.holes_visited;
        if !holes.contains(&target) {
            holes.push(target);
        }
        if !holes.contains(&exit) {
            holes.push(exit);
        }
    }
}

/// `OnPlayerLaunch` (`:415-447`), the `SpaceEnter` case: the player's ship
/// object, made from the player's ship and its archetype's mission
/// properties, the class `fighter` or `freighter` added once for a ship of
/// that type, the player's system record; its constructor gives the ship
/// the `PLAYER` state graph (`pub::AI::SubmitState` with no callbacks),
/// then it replaces the old ship object. Nothing for a player without a
/// state, where the C++ dereferences null (D801).
fn player_launch(cx: &mut Cx<'_>, player: u32) {
    let ship = api::player::ship(cx, player).unwrap_or(0);
    let archetype = api::player::ship_id(cx, player).unwrap_or(0);
    let mut class_ids = Vec::new();
    if let Some(properties) = cx
        .logic
        .content
        .tables
        .ship_mission_properties
        .get(&archetype)
    {
        for &p in properties {
            if !class_ids.contains(&p) {
                class_ids.push(p);
            }
        }
    }
    let Some(system) = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map(|s| s.system)
    else {
        return;
    };
    let kind = api::space_obj::object_type(cx, ship).unwrap_or(0);
    let class = if kind & 0x1_0000 != 0 {
        Some(fighter_class())
    } else if kind & 0x2_0000 != 0 {
        Some(freighter_class())
    } else {
        None
    };
    if let Some(class) = class
        && !class_ids.contains(&class)
    {
        class_ids.push(class);
    }
    let state_graph = cx
        .world
        .data()
        .state_graphs
        .find(b"PLAYER", GraphType::Leader);
    let op = SetPersonalityParams {
        state_graph,
        content_callback: CallbackArg::Null,
        directive_callback: CallbackArg::Null,
        set_personality: false,
        ..SetPersonalityParams::default()
    };
    let _ = api::ai::submit_state(
        cx,
        ship,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(op))),
    );
    if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
        state.ship = Some(PlayerShip {
            ship,
            archetype,
            class_ids,
            system,
            formation_id: u32::MAX,
            lane_entry: 0,
            lane_end: 0,
            in_combat: false,
            scan_target: false,
            target: 0,
            calm_since: -1.0,
            hostile_contact_count: 0,
        });
    }
}

/// The `Heartbeat` case: `GGameSupportElapsed = GGameSupportElapsed +
/// DeltaTime`, a `double` plus a `float`, exact in `f64` (T11); then every
/// player state's play time in key order (`UpdateAllContentPlayerTimes`).
pub fn heartbeat(cx: &mut Cx<'_>, elapsed: f32) {
    let state = &mut cx.logic.content.state;
    state.game_time += f64::from(elapsed);
    for player in state.players.values_mut() {
        player.update_time(elapsed);
    }
}

/// `GetContentGameTime`: the time GameSupport's heartbeats have added up.
#[must_use]
pub fn game_time(cx: &Cx<'_>) -> f64 {
    cx.logic.content.state.game_time
}

/// `GetContentTimeDistance(A, B)`: [`NO_TIME`] when either time is
/// [`NO_TIME`] (the C++ compares both dwords of each `double`), else
/// `|a - b|`.
#[must_use]
pub fn time_distance(a: f64, b: f64) -> f64 {
    if a.to_bits() == NO_TIME.to_bits() || b.to_bits() == NO_TIME.to_bits() {
        return NO_TIME;
    }
    (a - b).abs()
}
