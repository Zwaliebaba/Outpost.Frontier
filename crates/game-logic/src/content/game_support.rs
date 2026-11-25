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
//!   paths) and `SaveGame` (the gate locks); the best path over the system
//!   records' lanes ([`super::space_paths`]) and the player's statistics.
//!
//! A base's bar characters on `BaseEnter` and `BaseExit`
//! (`FContentBase::OnPlayerEnter`, `OnPlayerExit`) are BaseSupport's run
//! time ([`super::bar`], P3.6e).

use std::io::Write;
use std::sync::Arc;

use game_sim::ai::state_graph::GraphType;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use super::players::{self, PlayerShip, PlayerState};
use super::space_paths::{self, NnPathLeg};
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
                super::bar::on_player_enter(cx, base, player);
            }
        }
        ControllerMessage::BaseExit { base, player } => {
            let (base, player) = (*base, *player);
            if cx.logic.content.state.players.contains_key(&player) {
                if cx.logic.content.state.bases.contains_key(&base) {
                    super::bar::on_player_exit(cx, base, player);
                }
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
        ControllerMessage::RequestPlayerStats { player } => {
            let player = *player;
            player_stats(cx, player);
        }
        ControllerMessage::RequestBestPath { player, words } => {
            let (player, words) = (*player, *words);
            best_path(cx, player, &words);
        }
        _ => {}
    }
}

/// One point of the best path's reply (`FBestPathPoint`, 0x14 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
struct BestPathPoint {
    position: Vector,
    object: u32,
    system: u32,
}

/// `AppendLegPoints(Points, &Index, &Last, Leg, SystemId)` (`:155-177`):
/// the leg's start point unless it equals the previous end point (any
/// unequal or unordered component adds it), dropped again when it is the
/// first point and has no object; then its end point. The index of the last
/// point written.
fn append_leg_points(
    points: &mut Vec<BestPathPoint>,
    last: &mut Vector,
    leg: &NnPathLeg,
    system: u32,
) -> usize {
    let a = leg.point_a;
    if !(last.x == a.x && last.y == a.y && last.z == a.z)
        && (!points.is_empty() || leg.object_a != 0)
    {
        points.push(BestPathPoint {
            position: a,
            object: leg.object_a,
            system,
        });
    }
    points.push(BestPathPoint {
        position: leg.point_b,
        object: leg.object_b,
        system,
    });
    *last = leg.point_b;
    points.len() - 1
}

/// The reply block of `pub::Player::ReturnBestPath`: the 0xc-byte header
/// (the client's cookie, the point count, the flag byte and three zero
/// bytes), then the points, little-endian.
fn best_path_block(cookie: u32, flag: bool, points: &[BestPathPoint]) -> Vec<u8> {
    let mut out = Vec::with_capacity(12 + points.len() * 20);
    out.extend_from_slice(&cookie.to_le_bytes());
    out.extend_from_slice(&(points.len() as i32).to_le_bytes());
    out.extend_from_slice(&[u8::from(flag), 0, 0, 0]);
    for p in points {
        for v in [p.position.x, p.position.y, p.position.z] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&p.object.to_le_bytes());
        out.extend_from_slice(&p.system.to_le_bytes());
    }
    out
}

/// The cap of the best path across systems: the reply's 99 points.
const BEST_PATH_MAX_POINTS: usize = 0x63;

/// `OnBestPathRequest` (`:179-413`), the `RequestBestPath` case. The
/// request's words are the client's cookie, the start system and point,
/// the end point and the end system.
///
/// - **One system**: the system record (none: no reply) is loaded for the
///   request (`Load(false)`), the trip planned over its lanes with a range
///   of 500 ([`space_paths::build_best_path_legs`], the straight flight
///   when there is no path), each leg's points appended, the flag set when
///   there was no path, and the record released.
/// - **Across systems**: the gate path (type 1) and the gates locked for
///   the player; a route of fewer than two systems answers no points with
///   the flag. Otherwise each system of the route is loaded in turn, entered
///   at the request's start or the jump in from the previous system (a
///   locked entry gate fails), left at the request's end or the jump out to
///   the next, planned with a range of 1 ([`space_paths::build_nn_path`],
///   no path fails and the walk goes on), its points appended while fewer
///   than 99, the last one written taking the exit connection as its
///   object, and released. A failure sends no points. A failure at a
///   locked gate, or after one, breaks the walk before the record is
///   released, as the C++ does.
fn best_path(cx: &mut Cx<'_>, player: u32, words: &[u32; 9]) {
    let cookie = words[0];
    let from_system = words[1];
    let vector = |i: usize| {
        Vector::new(
            f32::from_bits(words[i]),
            f32::from_bits(words[i + 1]),
            f32::from_bits(words[i + 2]),
        )
    };
    let (from, to) = (vector(2), vector(5));
    let to_system = words[8];
    if from_system == to_system {
        if !cx.logic.content.state.systems.contains_key(&from_system) {
            return;
        }
        systems::load(cx, from_system, false);
        let Some(record) = cx.logic.content.state.systems.get(&from_system) else {
            return;
        };
        let (path, legs) = space_paths::build_best_path_legs(record, from, to, 500.0);
        let mut points = Vec::new();
        let mut last = Vector::ZERO;
        for leg in &legs {
            append_leg_points(&mut points, &mut last, leg, from_system);
        }
        let block = best_path_block(cookie, !path, &points);
        api::player::return_best_path(cx, player, &block);
        if let Some(record) = cx.logic.content.state.systems.get_mut(&from_system) {
            systems::unload(record);
        }
        return;
    }
    let paths = std::mem::take(&mut cx.logic.content.state.system_paths);
    let take = super::NoDispatch::begin(&cx.logic.content);
    let route = paths.path(from_system, to_system, 1);
    let locked = cx.logic.content.state.gate_locks.collect(player);
    if route.len() < 2 {
        take.end(&cx.logic.content);
        cx.logic.content.state.system_paths = paths;
        let block = best_path_block(cookie, true, &[]);
        api::player::return_best_path(cx, player, &block);
        return;
    }
    let mut current = from_system;
    let mut previous = current;
    let mut next_index = 1;
    let mut points: Vec<BestPathPoint> = Vec::new();
    let mut failed = false;
    loop {
        let next = if next_index < route.len() {
            next_index += 1;
            route[next_index - 1]
        } else {
            to_system
        };
        if !cx.logic.content.state.systems.contains_key(&current) {
            failed = true;
            break;
        }
        systems::load(cx, current, false);
        let start = if current == from_system {
            from
        } else {
            let entry =
                super::system_paths::next_hop_connection_object(cx, &paths, current, previous, 1);
            let position = super::system_paths::connection_position(cx, current, entry)
                .unwrap_or(Vector::ZERO);
            if !locked.is_empty() {
                if locked.contains(&entry) {
                    failed = true;
                    break;
                }
                if failed {
                    break;
                }
            }
            position
        };
        let mut exit = 0;
        let mut exit_is_connection = false;
        let end = if current == to_system {
            to
        } else {
            exit = super::system_paths::next_hop_connection_object(cx, &paths, current, next, 1);
            exit_is_connection = true;
            super::system_paths::connection_position(cx, current, exit).unwrap_or(Vector::ZERO)
        };
        if let Some(record) = cx.logic.content.state.systems.get(&current) {
            let (path, legs) = space_paths::build_nn_path(record, start, end, 1.0);
            if path {
                let mut last_point = None;
                let mut last = Vector::ZERO;
                for leg in &legs {
                    if points.len() >= BEST_PATH_MAX_POINTS {
                        break;
                    }
                    last_point = Some(append_leg_points(&mut points, &mut last, leg, current));
                }
                if let Some(i) = last_point
                    && exit_is_connection
                {
                    points[i].object = exit;
                }
            } else {
                failed = true;
            }
        }
        if let Some(record) = cx.logic.content.state.systems.get_mut(&current) {
            systems::unload(record);
        }
        if current == next {
            break;
        }
        previous = current;
        current = next;
        if points.len() >= BEST_PATH_MAX_POINTS {
            break;
        }
    }
    take.end(&cx.logic.content);
    cx.logic.content.state.system_paths = paths;
    if failed {
        points.clear();
    }
    let block = best_path_block(cookie, failed, &points);
    api::player::return_best_path(cx, player, &block);
}

/// `OnPlayerStatsRequest` (`:478-541`), the `RequestPlayerStats` case: for
/// a player with a state, `pub::Player::ReturnPlayerStats` with the 0x34-byte
/// header (the random missions completed, of type 7 and 0 to 5, aborted
/// and failed of types 0 to 5; the cash earned and time played as `float`s
/// of their `ftol`; the systems, bases and holes visited; the count of ship
/// types killed; the rank, in single player the player's, else `ftol` of
/// the vibe's; the worth, cash plus `ftol` of the asset value; the money to
/// the next rank; the group count), the `{ship type, count}` pairs in key
/// order, then every faction's `{group, feeling}` of the player's vibe (0
/// when the call fails), little-endian.
fn player_stats(cx: &mut Cx<'_>, player: u32) {
    let Some(state) = cx.logic.content.state.players.get(&player).cloned() else {
        return;
    };
    let vibe = api::player::rep(cx, player).unwrap_or(-1);
    let mut groups = Vec::new();
    api::reputation::enumerate_groups(cx, |group| {
        groups.push(group);
        true
    });
    let feelings: Vec<(u32, f32)> = groups
        .into_iter()
        .map(|g| {
            (
                g,
                api::reputation::group_feelings_towards(cx, vibe, g).unwrap_or(0.0),
            )
        })
        .collect();
    let stats = &state.stats;
    let count =
        |map: &std::collections::BTreeMap<u8, i32>, kind: u8| map.get(&kind).copied().unwrap_or(0);
    let mut completed = count(&stats.random_missions_completed, 7);
    let (mut aborted, mut failed) = (0i32, 0i32);
    for kind in 0..6 {
        completed = completed.wrapping_add(count(&stats.random_missions_completed, kind));
        aborted = aborted.wrapping_add(count(&stats.random_missions_aborted, kind));
        failed = failed.wrapping_add(count(&stats.random_missions_failed, kind));
    }
    let rank = if api::misc::single_player(cx) {
        api::player::rank(cx, player).unwrap_or(0)
    } else {
        let rank = api::reputation::rank(cx, vibe).unwrap_or(0.0);
        neuron_core::num::trunc_to_i32(f64::from(rank))
    };
    let cash = api::player::inspect_cash(cx, player).unwrap_or(0);
    let asset = api::player::asset_value(cx, player).unwrap_or(0.0);
    let worth = cash.wrapping_add(neuron_core::num::trunc_to_i32(f64::from(asset)));
    let money = api::player::money_needed_to_next_rank(cx, player).unwrap_or(0);
    let total = |v: f32| neuron_core::num::trunc_to_i32(f64::from(v)) as f32;
    let mut block = Vec::new();
    let mut int = |v: i32| block.extend_from_slice(&v.to_le_bytes());
    int(completed);
    int(aborted);
    int(failed);
    int(total(stats.total_cash_earned).to_bits().cast_signed());
    int(total(stats.total_time_played).to_bits().cast_signed());
    int(stats.systems_visited.len() as i32);
    int(stats.bases_visited.len() as i32);
    int(stats.holes_visited.len() as i32);
    int(stats.ship_types_killed.len() as i32);
    int(rank);
    int(worth);
    int(money);
    int(feelings.len() as i32);
    for (&ship, &kills) in &stats.ship_types_killed {
        int(ship.cast_signed());
        int(kills);
    }
    for &(group, feeling) in &feelings {
        int(group.cast_signed());
        int(feeling.to_bits().cast_signed());
    }
    api::player::return_player_stats(cx, player, &block);
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
        state.ship = Some(PlayerShip::new(ship, archetype, class_ids, system));
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
