//! The formation helpers of TU53 (`Content/DockQueues.cpp:186-433`,
//! `:1086-1376`, `:1475-1590`; work package P3.6a, decision record 0008
//! point 7) and the formation id counter (`GLastFormationId`,
//! `Content/GameSupport.cpp:770-777`).
//!
//! A formation is the set of Content ships ([`super::ships`]) that share a
//! `FormationId`; slot 0 is its leader, and each ship flies at its slot of
//! a `formations.ini` record ([`super::ContentTables::formations`]). The
//! AI follows through `pub::AI::SubmitState` (the `LEADER` or `ESCORT`
//! graph) and `pub::AI::update_formation_state` ([`reset_formation_state`]).
//!
//! Until P3.6c the Content ships are the players' ships, so the system
//! records' space objects that the C++ walks first are empty here, and the
//! NPC members' parts (`FollowDirectivesOf`, the guides) are P3.6c's.

use game_sim::ai::behavior::{DirectiveOp, DockOp};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::vc6::vc6_sort;
use neuron_core::id::create_id;
use neuron_core::math::Vector;

use super::ContentState;
use super::dock_queues;
use super::mission::obj_list::DirectiveKind;
use super::players;
use super::ships::{self, ShipKey};
use crate::api;
use crate::api::ai::{BaseOp, Op, SetPersonalityParams};
use crate::events::NpcShipKey;
use crate::server::Cx;

/// `AllocateFormationId` (`Content/GameSupport.cpp:774-777`): the counter
/// pre-incremented.
pub fn allocate_id(state: &mut ContentState) -> u32 {
    state.last_formation_id = state.last_formation_id.wrapping_add(1);
    state.last_formation_id
}

/// `FFormation::IsPlayerSlot(Index)` (`Content/Encounters.cpp:133-136`): a
/// slot past the end, which the C++ reads unchecked, is no player slot
/// (D840).
pub fn is_player_slot(cx: &Cx<'_>, formation: Option<u32>, slot: i32) -> bool {
    let Some(f) = formation.and_then(|f| cx.logic.content.tables.formation(f)) else {
        return false;
    };
    usize::try_from(slot)
        .ok()
        .and_then(|s| f.slots.get(s))
        .is_some_and(|s| s.player_slot)
}

/// The slot count of a formation (`Slots.size()`); a null formation, which
/// the C++ dereferences, has none (D840).
pub fn slot_count(cx: &Cx<'_>, formation: Option<u32>) -> i32 {
    formation
        .and_then(|f| cx.logic.content.tables.formation(f))
        .map_or(0, |f| f.slots.len() as i32)
}

/// `FFormation::GetSlotPosition(Index)` (`Content/Encounters.cpp:92-108`):
/// the slot's position turned by the formation's orientation and moved by
/// its position, which the loader leaves the identity and zero; zero past
/// the end.
pub fn slot_position(cx: &Cx<'_>, formation: Option<u32>, slot: u32) -> Vector {
    let Some(f) = formation.and_then(|f| cx.logic.content.tables.formation(f)) else {
        return Vector::ZERO;
    };
    let Some(s) = f.slots.get(slot as usize) else {
        return Vector::ZERO;
    };
    // The identity times the position, plus the zero position: the
    // position itself, with -0 read as +0.
    Vector::new(
        s.position[0] + 0.0,
        s.position[1] + 0.0,
        s.position[2] + 0.0,
    )
}

/// `CollectFormationMembers(&Out, FormationId)` (`:186-218`): per system
/// record its space objects in the formation (none until P3.6c), then
/// every player state's ship in it, sorted by slot (signed) with the VC6
/// `std::sort`. Nothing for formation -1.
#[must_use]
pub fn collect_members(state: &ContentState, formation_id: u32) -> Vec<ShipKey> {
    if formation_id == u32::MAX {
        return Vec::new();
    }
    let mut out: Vec<ShipKey> = Vec::new();
    for record in state.systems.values() {
        for &k in &record.space_objs {
            if state
                .npc_ships
                .get(&k)
                .is_some_and(|s| s.base.formation_id == formation_id)
            {
                out.push(ShipKey::Npc(k));
            }
        }
    }
    out.extend(
        state
            .players
            .iter()
            .filter(|(_, p)| {
                p.ship
                    .as_ref()
                    .is_some_and(|s| s.formation_id == formation_id)
            })
            .map(|(&id, _)| ShipKey::Player(id)),
    );
    let slots: Vec<(ShipKey, i32)> = out
        .iter()
        .map(|&k| (k, ships::formation_slot(state, k) as i32))
        .collect();
    let mut sorted = slots;
    vc6_sort(&mut sorted, |a, b| a.1 < b.1);
    out = sorted.into_iter().map(|(k, _)| k).collect();
    out
}

/// `FindFormationLeader(FormationId)` (`:225-256`): per system record its
/// space objects, then every player's ship, the first in the formation at
/// slot 0 (or formation -1); none. The players are walked inside the
/// system loop, so without any system record there is no leader.
#[must_use]
pub fn find_leader(state: &ContentState, formation_id: u32) -> Option<ShipKey> {
    let leads = |s: &super::players::PlayerShip| {
        s.formation_id == formation_id && (s.formation_slot == 0 || s.formation_id == u32::MAX)
    };
    for record in state.systems.values() {
        for &k in &record.space_objs {
            if state.npc_ships.get(&k).is_some_and(|s| leads(&s.base)) {
                return Some(ShipKey::Npc(k));
            }
        }
        if let Some((&id, _)) = state
            .players
            .iter()
            .find(|(_, p)| p.ship.as_ref().is_some_and(leads))
        {
            return Some(ShipKey::Player(id));
        }
    }
    None
}

/// `CancelDockingBehindLeader(Ship, FormationId)` (`:258-279`): when the
/// formation's leader is docking at a terminal of type 7 or 8, the ship's
/// docking is cancelled (with the leader's target passed on).
pub fn cancel_docking_behind_leader(cx: &mut Cx<'_>, ship: ShipKey, formation_id: u32) {
    let state = &cx.logic.content.state;
    let Some(leader) = find_leader(state, formation_id) else {
        return;
    };
    let Some(entry) = dock_queues::docking_entry_of(state, ships::id(state, leader)) else {
        return;
    };
    // GetTerminalInfo's result is not tested (D841).
    let terminal =
        api::space_obj::terminal_info(cx, entry.target, entry.index).map_or(0, |i| i.value);
    if terminal == 7 || terminal == 8 {
        let id = ships::id(&cx.logic.content.state, ship);
        dock_queues::cancel_ship_docking(cx, id, entry.target, false);
    }
}

/// `pub::StateGraph::get_state_graph(get_state_graph_id(Ship), Type)`: the
/// state graph of the ship's own graph name for that type.
fn state_graph(cx: &mut Cx<'_>, ship: u32, kind: GraphType) -> i32 {
    let current = api::ai::state_graph_id(cx, ship);
    let graphs = &cx.world.data().state_graphs;
    graphs.find_by_id(current, kind)
}

/// `ResetFormationState(Ship)` (`:286-321`): the slot's offset (zero
/// without a formation); a wingman with a leader gets the `ESCORT` graph
/// and follows the leader at the offset (without a leader nothing is
/// sent); anyone else gets the `LEADER` graph and leads itself (or no one,
/// outside a formation).
pub fn reset_formation_state(cx: &mut Cx<'_>, ship: ShipKey) {
    let state = &cx.logic.content.state;
    let formation_id = ships::formation_id(state, ship);
    let slot = ships::formation_slot(state, ship);
    let formation = ships::formation(state, ship);
    let id = ships::id(state, ship);
    let offset = if formation.is_some() {
        slot_position(cx, formation, slot)
    } else {
        Vector::ZERO
    };
    if formation_id != u32::MAX && slot != 0 {
        let Some(leader) = find_leader(&cx.logic.content.state, formation_id) else {
            return;
        };
        let leader_id = ships::id(&cx.logic.content.state, leader);
        submit_graph(cx, id, GraphType::Escort);
        let _ = api::ai::update_formation_state(cx, id, leader_id, offset);
        return;
    }
    submit_graph(cx, id, GraphType::Leader);
    let leader = if formation_id == u32::MAX { 0 } else { id };
    let _ = api::ai::update_formation_state(cx, id, leader, offset);
}

fn submit_graph(cx: &mut Cx<'_>, ship: u32, kind: GraphType) {
    let state_graph = state_graph(cx, ship, kind);
    let op = SetPersonalityParams {
        state_graph,
        ..SetPersonalityParams::default()
    };
    let _ = api::ai::submit_state(
        cx,
        ship,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(op))),
    );
}

/// The member looked up again in its own system record
/// (`Member->GetSystem()->FindShip(Id)`); a ship without a system, which
/// the C++ dereferences, is not found (D843).
fn live_member(cx: &Cx<'_>, member: ShipKey) -> Option<ShipKey> {
    let state = &cx.logic.content.state;
    let system = ships::system(state, member)?;
    ships::find_in_system(cx, system, ships::id(state, member))
}

/// `RemoveFromFormation(Ship)` (`:326-353`): a leader dissolves its
/// formation (each live member still in one cancels its docking behind the
/// leader and leaves; every member's state is reset); a member leaves
/// (`SetFormation(-1, -1, null)`) and is reset; a ship in no formation
/// does nothing.
pub fn remove_from_formation(cx: &mut Cx<'_>, ship: ShipKey) {
    let formation_id = ships::formation_id(&cx.logic.content.state, ship);
    if find_leader(&cx.logic.content.state, formation_id) == Some(ship) {
        let members = collect_members(&cx.logic.content.state, formation_id);
        for member in members {
            if let Some(live) = live_member(cx, member) {
                let live_formation = ships::formation_id(&cx.logic.content.state, live);
                if live_formation != u32::MAX {
                    cancel_docking_behind_leader(cx, live, live_formation);
                    ships::set_formation(&mut cx.logic.content.state, live, u32::MAX, 0, None);
                }
            }
            reset_formation_state(cx, member);
        }
        return;
    }
    if formation_id == u32::MAX {
        return;
    }
    ships::set_formation(&mut cx.logic.content.state, ship, u32::MAX, u32::MAX, None);
    reset_formation_state(cx, ship);
}

/// `ReleaseFormationMembers(Ship, &Ships)` (`:358-376`): every other
/// member of the ship's formation leaves it, the players' ships appended
/// to `ships` first.
pub fn release_members(cx: &mut Cx<'_>, ship: ShipKey, out: &mut Vec<ShipKey>) {
    let state = &cx.logic.content.state;
    let own = ships::id(state, ship);
    let members = collect_members(state, ships::formation_id(state, ship));
    for member in members {
        let state = &cx.logic.content.state;
        let id = ships::id(state, member);
        if id == own {
            continue;
        }
        if let Some(player) = players::find_by_ship(&state.players, id)
            && state.players.get(&player).is_some_and(|p| p.ship.is_some())
        {
            out.push(ShipKey::Player(player));
        }
        remove_from_formation(cx, member);
    }
}

/// `FormFormation(&Ships, Formation, bReleaseLeaderFollowers)`
/// (`:384-433`): the ships become a new formation laid out by `formation`.
/// The first ship, an NPC ship in a formation, releases its formation's
/// other members into the list first; the NPC ships take the non-player
/// slots in list order (a leader releasing its members first with
/// `release_leader_followers`), the players' ships the first player slot
/// from there.
pub fn form_formation(
    cx: &mut Cx<'_>,
    list: &mut Vec<ShipKey>,
    formation: Option<u32>,
    release_leader_followers: bool,
) {
    let Some(&first) = list.first() else {
        return;
    };
    if ships::formation_id(&cx.logic.content.state, first) != u32::MAX && ships::is_npc(first) {
        release_members(cx, first, list);
    }
    let id = allocate_id(&mut cx.logic.content.state);
    let mut slot = 0_i32;
    let mut at = 0;
    while at < list.len() {
        let ship = list[at];
        at += 1;
        let ship_id = ships::id(&cx.logic.content.state, ship);
        if players::find_by_ship(&cx.logic.content.state.players, ship_id).is_some() {
            continue;
        }
        if is_player_slot(cx, formation, slot) {
            continue;
        }
        if release_leader_followers && ships::formation_slot(&cx.logic.content.state, ship) == 0 {
            release_members(cx, ship, list);
        }
        ships::set_formation(
            &mut cx.logic.content.state,
            ship,
            id,
            slot as u32,
            formation,
        );
        reset_formation_state(cx, ship);
        slot += 1;
    }
    let count = slot_count(cx, formation);
    let mut at = 0;
    while at < list.len() {
        let ship = list[at];
        at += 1;
        let ship_id = ships::id(&cx.logic.content.state, ship);
        if players::find_by_ship(&cx.logic.content.state.players, ship_id).is_none() {
            continue;
        }
        if !is_player_slot(cx, formation, slot) {
            while slot < count {
                slot += 1;
                if is_player_slot(cx, formation, slot) {
                    break;
                }
            }
        }
        if slot < count {
            ships::set_formation(
                &mut cx.logic.content.state,
                ship,
                id,
                slot as u32,
                formation,
            );
            reset_formation_state(cx, ship);
        }
    }
}

/// `IsPlayerInShipFormation(System, Ship)` (`:1114-1129`): a player's ship,
/// or a ship with a player's ship among its formation's members. A ship
/// the record does not know, which the C++ dereferences, has no formation
/// (D843).
#[must_use]
pub fn is_player_in_ship_formation(cx: &Cx<'_>, system: u32, ship: u32) -> bool {
    let state = &cx.logic.content.state;
    if players::find_by_ship(&state.players, ship).is_some() {
        return true;
    }
    let formation_id =
        ships::find_in_system(cx, system, ship).map_or(u32::MAX, |k| ships::formation_id(state, k));
    collect_members(state, formation_id)
        .iter()
        .any(|&m| players::find_by_ship(&state.players, ships::id(state, m)).is_some())
}

/// `FollowLeaderIntoDock(Ship)` (`:1136-1170`): a wingman whose leader is
/// docking at a terminal of type 7 or 8 enters the docking list behind it
/// and is told to dock there (`Dock(.., 4)`). A wingman without a leader,
/// which the C++ dereferences, does nothing (D843).
pub fn follow_leader_into_dock(cx: &mut Cx<'_>, ship: ShipKey) {
    let state = &cx.logic.content.state;
    if !ships::is_wingman(state, ship) {
        return;
    }
    let Some(leader) = find_leader(state, ships::formation_id(state, ship)) else {
        return;
    };
    let Some(entry) = dock_queues::docking_entry_of(state, ships::id(state, leader)) else {
        return;
    };
    let terminal =
        api::space_obj::terminal_info(cx, entry.target, entry.index).map_or(0, |i| i.value);
    if terminal != 7 && terminal != 8 {
        return;
    }
    let state = &cx.logic.content.state;
    let system = ships::system(state, ship).unwrap_or(0);
    let id = ships::id(state, ship);
    let time = super::game_support::game_time(cx);
    let q = &mut cx.logic.content.state.dock_queues;
    let mut new = q.new_entry(time);
    new.system = system;
    new.ship = id;
    new.target = entry.target;
    new.index = entry.index;
    q.docking.entries.push(new);
    let _ = api::space_obj::dock(
        cx,
        id,
        entry.target,
        entry.index,
        dock_queues::RESPONSE_PROCEED,
    );
}

/// `HandleFormationRequest(&Ship, &Leader, System)` (`:1192-1349`): the
/// ship asks to join the leader's formation. 0 joined; -4 no leader ship,
/// -3 no requesting ship, -1 already together, -8 a formation record
/// missing, the formation's first member a wingman, or players not
/// grouped, -9 the formation docks at a gate locked for the player (or a
/// station or planet), -5 no free slot.
pub fn handle_formation_request(cx: &mut Cx<'_>, ship: u32, leader: u32, system: u32) -> i32 {
    let Some(leader_key) = ships::find_in_system(cx, system, leader) else {
        return -4;
    };
    let Some(ship_key) = ships::find_in_system(cx, system, ship) else {
        return -3;
    };
    let state = &cx.logic.content.state;
    let mut leader_formation = ships::formation_id(state, leader_key);
    let ship_formation = ships::formation_id(state, ship_key);
    let ship_leader = ships::formation_leader(state, ship_key);
    let leader_leader = ships::formation_leader(state, leader_key);
    let ship_player = players::find_by_ship(&state.players, ship);
    let leader_player = players::find_by_ship(&state.players, leader);
    let player_rules =
        !api::misc::single_player(cx) && (ship_player.is_some() || leader_player.is_some());

    let multiplayer_freighter = create_id(b"multiplayer_freighter");
    let freighter_basic = create_id(b"freighter_basic");
    let tables = &cx.logic.content.tables;
    if tables.formation(multiplayer_freighter).is_none()
        || tables.formation(freighter_basic).is_none()
    {
        return -8;
    }

    if !player_rules {
        if leader_formation == u32::MAX {
            leader_formation = allocate_id(&mut cx.logic.content.state);
            ships::set_formation(
                &mut cx.logic.content.state,
                leader_key,
                leader_formation,
                0,
                Some(freighter_basic),
            );
            reset_formation_state(cx, leader_key);
        }
        if ship_formation == leader_formation {
            return -1;
        }
        let state = &cx.logic.content.state;
        let members = collect_members(state, leader_formation);
        // The front is read without an empty test; the leader was just
        // found in its system, so it is a member (D840 for the rest).
        let Some(&first) = members.first() else {
            return -8;
        };
        if ships::is_wingman(state, first) {
            return -8;
        }
        let player = players::find_by_ship(&state.players, ship);
        if let Some((target, _, _)) = dock_queues::ship_docking_target(cx, ships::id(state, first))
            && target & 0x3fff_ffff != 0
        {
            let target_type = api::space_obj::object_type(cx, target).unwrap_or(0);
            if let Some(player) = player
                && (cx.logic.content.state.gate_locks.is_locked(player, target)
                    || target_type & 0x120 != 0)
            {
                return -9;
            }
        }
        let state = &cx.logic.content.state;
        let formation = ships::formation(state, first);
        let first_formation_id = ships::formation_id(state, first);
        let taken: Vec<u32> = members
            .iter()
            .map(|&m| ships::formation_slot(state, m))
            .collect();
        for slot in 0..slot_count(cx, formation) {
            if taken.contains(&(slot as u32)) {
                continue;
            }
            let player_ship =
                players::find_by_ship(&cx.logic.content.state.players, ship).is_some();
            if player_ship != is_player_slot(cx, formation, slot) {
                continue;
            }
            ships::set_formation(
                &mut cx.logic.content.state,
                ship_key,
                first_formation_id,
                slot as u32,
                formation,
            );
            return 0;
        }
        return -5;
    }

    if let (Some(a), Some(b)) = (ship_player, leader_player)
        && !api::player::is_group_member(cx, a, b)
    {
        return -8;
    }
    if ship_leader == leader_leader {
        return -1;
    }
    let formation = Some(multiplayer_freighter);
    if leader_formation == u32::MAX {
        leader_formation = allocate_id(&mut cx.logic.content.state);
        ships::set_formation(
            &mut cx.logic.content.state,
            leader_key,
            leader_formation,
            0,
            formation,
        );
        reset_formation_state(cx, leader_key);
    }
    let joining = if ship_leader == ship_key && ship_formation != u32::MAX {
        let joining = collect_members(&cx.logic.content.state, ship_formation);
        for &member in &joining {
            let Some(live) = live_member(cx, member) else {
                continue;
            };
            let live_formation = ships::formation_id(&cx.logic.content.state, live);
            if live_formation == u32::MAX {
                continue;
            }
            cancel_docking_behind_leader(cx, live, live_formation);
            ships::set_formation(&mut cx.logic.content.state, live, u32::MAX, 0, None);
        }
        joining
    } else {
        vec![ship_key]
    };
    let state = &cx.logic.content.state;
    let taken: Vec<u32> = collect_members(state, leader_formation)
        .iter()
        .map(|&m| ships::formation_slot(state, m))
        .collect();
    let count = slot_count(cx, formation);
    for join in joining {
        if count <= 0 {
            return -5;
        }
        let mut slot = 0;
        while taken.contains(&(slot as u32)) {
            slot += 1;
            if slot >= count {
                return -5;
            }
        }
        ships::set_formation(
            &mut cx.logic.content.state,
            join,
            leader_formation,
            slot as u32,
            formation,
        );
        reset_formation_state(cx, join);
    }
    0
}

/// `DropPlayersLockedOutOfGate(&Ship, &Target)` (`:1546-1573`): in the
/// ship's formation (its system record must know it), each player for whom
/// the target is a locked gate leaves the formation.
pub fn drop_players_locked_out_of_gate(cx: &mut Cx<'_>, ship: u32, target: u32) {
    if ship & 0x3fff_ffff == 0 {
        return;
    }
    let system = api::space_obj::system(cx, ship).unwrap_or(0);
    let Some(key) = ships::find_in_system(cx, system, ship) else {
        return;
    };
    let formation_id = ships::formation_id(&cx.logic.content.state, key);
    if formation_id == u32::MAX {
        return;
    }
    let members = collect_members(&cx.logic.content.state, formation_id);
    for member in members {
        let state = &cx.logic.content.state;
        let Some(player) = players::find_by_ship(&state.players, ships::id(state, member)) else {
            continue;
        };
        if state.gate_locks.is_locked(player, target) {
            remove_from_formation(cx, member);
        }
    }
}

/// `DockFormationFollowers(&System, &Ship, &Target, &Index)`
/// (`:1086-1108`): every member after the first (the leader) enters the
/// docking list at the leader's terminal and is told to dock there. A ship
/// the record does not know, which the C++ dereferences, has no followers
/// (D843).
pub fn dock_formation_followers(cx: &mut Cx<'_>, system: u32, ship: u32, target: u32, index: i32) {
    let Some(key) = ships::find_in_system(cx, system, ship) else {
        return;
    };
    let state = &cx.logic.content.state;
    let members: Vec<u32> = collect_members(state, ships::formation_id(state, key))
        .iter()
        .map(|&m| ships::id(state, m))
        .collect();
    for &member in members.iter().skip(1) {
        let time = super::game_support::game_time(cx);
        let q = &mut cx.logic.content.state.dock_queues;
        let mut entry = q.new_entry(time);
        entry.system = system;
        entry.ship = member;
        entry.target = target;
        entry.index = index;
        q.docking.entries.push(entry);
        let _ = api::space_obj::dock(cx, member, target, index, dock_queues::RESPONSE_PROCEED);
    }
}

/// `BreakFormationAtDock(&System, &Ship, bRedirectPlayers,
/// bReleasePlayers)` (`:1475-1543`): a leader (or a ship without a
/// formation) of a formation of more than one member that docks at a
/// station breaks it up. The NPC members leave it, take the leader's
/// directives (`FollowDirectivesOf`) and a clone of its guide. With either
/// flag the players' ships leave it too, and with `redirect_players` each
/// is sent to dock: the leader's first directive when it is a dock
/// directive (at its own docking entry's terminal when it has one), and
/// with no directive at that terminal.
pub fn break_formation_at_dock(
    cx: &mut Cx<'_>,
    system: u32,
    ship: u32,
    redirect_players: bool,
    release_players: bool,
) {
    // FindSystemInfo is not null-tested (D843).
    let Some(leader) = ships::find_in_system(cx, system, ship) else {
        return;
    };
    let state = &cx.logic.content.state;
    if ships::is_wingman(state, leader) {
        return;
    }
    let members = collect_members(state, ships::formation_id(state, leader));
    if members.len() <= 1 {
        return;
    }
    // Members after the first that the record's space objects hold are the
    // NPC members; the rest are players' ships.
    let mut npc_members: Vec<NpcShipKey> = Vec::new();
    let mut player_members: Vec<ShipKey> = Vec::new();
    for &m in &members[1..] {
        let id = ships::id(state, m);
        if let Some(k) = ships::find_space_obj(state, system, id) {
            npc_members.push(k);
        } else if let Some(p) = players::find_by_ship(&state.players, id) {
            player_members.push(ShipKey::Player(p));
        }
    }
    for member in npc_members {
        remove_from_formation(cx, ShipKey::Npc(member));
        super::npc_ships::follow_directives_of(cx, ShipKey::Npc(member), leader);
        // A player's ship as leader has no guide (the C++ reads one at its
        // `+0x178` all the same, D854).
        let ShipKey::Npc(lead) = leader else {
            continue;
        };
        let Some(guide) = cx
            .logic
            .content
            .state
            .npc_ships
            .get(&lead)
            .and_then(|s| s.guide)
        else {
            continue;
        };
        if let Some(copy) = super::guides::clone_guide(cx, guide) {
            super::guides::set_guide(cx, member, Some(copy), true, false);
            super::guides::release(cx, copy);
        }
    }
    if !release_players && !redirect_players {
        return;
    }
    for &member in &player_members {
        remove_from_formation(cx, member);
    }
    if !redirect_players || player_members.is_empty() {
        return;
    }
    for member in player_members {
        let mut directives = super::npc_ships::copy_directives(cx, leader);
        let id = ships::id(&cx.logic.content.state, member);
        let entry = dock_queues::docking_entry_of(&cx.logic.content.state, id);
        let op = if let Some(&first) = directives.first() {
            match cx
                .logic
                .content
                .state
                .directives
                .get(first)
                .map(|d| &d.kind)
            {
                Some(DirectiveKind::Dock(dock)) => {
                    let mut op = dock.op.clone();
                    if let Some(entry) = &entry {
                        op.dock_object = entry.target;
                        op.field_14 = 1;
                        op.port = entry.index;
                    }
                    Some((dock.priority, op))
                }
                // A first directive of another type (an instant trade lane
                // answers 0x13 too, D854) sends nothing.
                _ => None,
            }
        } else {
            entry.map(|entry| {
                (
                    1,
                    DockOp {
                        dock_object: entry.target,
                        field_14: 1,
                        port: entry.index,
                        ..DockOp::default()
                    },
                )
            })
        };
        if let Some((priority, op)) = op {
            let mut directive = BaseOp::directive(DirectiveOp::Dock(op));
            directive.priority = priority;
            let _ = api::ai::submit_directive(cx, id, &directive);
        }
        super::directives::clear_list(&mut cx.logic.content.state.directives, &mut directives);
    }
}

/// `FSpaceSupport::PickFormationLeader(&Ships)` (`SpaceSupport.cpp:168-222`):
/// of the NPC ships of the list, a large ship (`LARGE_SHIPS`) beats any
/// other, else the strictly higher rank; the leader moves to the list's
/// front. `None` when the list has no NPC ship.
pub fn pick_formation_leader(cx: &mut Cx<'_>, list: &mut Vec<ShipKey>) -> Option<ShipKey> {
    let large = create_id(b"LARGE_SHIPS");
    let mut best: Option<(usize, ShipKey)> = None;
    for (at, &candidate) in list.clone().iter().enumerate() {
        if !ships::is_npc(candidate) {
            continue;
        }
        let Some((_, b)) = best else {
            best = Some((at, candidate));
            continue;
        };
        let mut compare_ranks = false;
        if ships::is_in_ship_class(cx, candidate, large) {
            if !ships::is_in_ship_class(cx, b, large) {
                best = Some((at, candidate));
                continue;
            }
            compare_ranks = true;
        } else if !ships::is_in_ship_class(cx, b, large) {
            compare_ranks = true;
        }
        if compare_ranks {
            let candidate_rank = ships::reputation_rank(cx, candidate);
            if ships::reputation_rank(cx, b) < candidate_rank {
                best = Some((at, candidate));
            }
        }
    }
    let (at, leader) = best?;
    list.remove(at);
    list.insert(0, leader);
    Some(leader)
}

/// `FSpaceSupport::PromoteFormationLeader(System, Ship)` (`:225-299`), a
/// killed NPC ship that led its formation: with no NPC wingman the
/// players' wingmen leave the formation; otherwise the NPC wingmen, then
/// the players', form a new formation of the same layout under the leader
/// [`pick_formation_leader`] chose, which takes the old leader's directives
/// and its guide, and `FormationLeaderChanged` goes to every controller.
/// True when a new leader took the guide.
pub fn promote_formation_leader(cx: &mut Cx<'_>, _system: u32, ship: NpcShipKey) -> bool {
    let key = ShipKey::Npc(ship);
    let state = &cx.logic.content.state;
    if !state.npc_ships.contains_key(&ship) || ships::is_wingman(state, key) {
        return false;
    }
    let members = collect_members(state, ships::formation_id(state, key));
    let mut npc_wingmen: Vec<ShipKey> = Vec::new();
    let mut player_wingmen: Vec<ShipKey> = Vec::new();
    for m in members {
        if !ships::is_wingman(state, m) {
            continue;
        }
        let id = ships::id(state, m);
        if players::find_by_ship(&state.players, id).is_none() {
            npc_wingmen.push(m);
        } else {
            player_wingmen.push(m);
        }
    }
    if npc_wingmen.is_empty() {
        for w in player_wingmen {
            // Looked up again in its system (`GetSystem()->FindShip`, not
            // null-tested, D843).
            let state = &cx.logic.content.state;
            let id = ships::id(state, w);
            let Some(system) = ships::system(state, w) else {
                continue;
            };
            let Some(found) = ships::find_in_system(cx, system, id) else {
                continue;
            };
            let formation = ships::formation_id(&cx.logic.content.state, found);
            if formation == u32::MAX {
                continue;
            }
            cancel_docking_behind_leader(cx, found, formation);
            ships::set_formation(&mut cx.logic.content.state, found, u32::MAX, 0, None);
        }
        return false;
    }
    npc_wingmen.extend(player_wingmen);
    let Some(new_leader) = pick_formation_leader(cx, &mut npc_wingmen) else {
        return false;
    };
    let layout = ships::formation(&cx.logic.content.state, key);
    form_formation(cx, &mut npc_wingmen, layout, false);
    super::npc_ships::follow_directives_of(cx, new_leader, key);
    let guide = cx
        .logic
        .content
        .state
        .npc_ships
        .get(&ship)
        .and_then(|s| s.guide);
    let ShipKey::Npc(new_key) = new_leader else {
        return false;
    };
    super::guides::set_guide(cx, new_key, guide, true, false);
    let _ = cx.dispatch(
        neuron_server::controller::ControllerId::BROADCAST,
        &mut crate::events::ControllerMessage::FormationLeaderChanged {
            new_leader: new_key,
            old_leader: ship,
        },
    );
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::players::{PlayerShip, PlayerState};

    fn with_ship(state: &mut ContentState, player: u32, ship: u32, formation: u32, slot: u32) {
        let mut p = PlayerState { player, ..dummy() };
        let mut s = PlayerShip::new(ship, 0, Vec::new(), None);
        s.formation_id = formation;
        s.formation_slot = slot;
        p.ship = Some(s);
        state.players.insert(player, p);
    }

    fn dummy() -> PlayerState {
        let mut world = game_sim::object::World::new(
            std::sync::Arc::new(game_sim::data::config::GameData::new()),
            game_sim::object::world::WorldConfig::default(),
        );
        let mut logic =
            crate::server::Logic::new(Box::new(crate::server::RecordingSink::default()), 1, true);
        let cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        PlayerState::new(&cx, 0)
    }

    #[test]
    fn members_sort_by_slot_and_leaders_need_a_system() {
        let mut state = ContentState::load();
        with_ship(&mut state, 1, 101, 5, 2);
        with_ship(&mut state, 2, 102, 5, 0);
        with_ship(&mut state, 3, 103, 6, 0);
        with_ship(&mut state, 4, 104, 5, 1);
        let ids: Vec<u32> = collect_members(&state, 5)
            .iter()
            .map(|&k| ships::id(&state, k))
            .collect();
        assert_eq!(ids, [102, 104, 101]);
        assert!(collect_members(&state, u32::MAX).is_empty());
        assert_eq!(find_leader(&state, 5), None);
        state
            .systems
            .insert(9, crate::content::systems::SystemInfo::new(9));
        assert_eq!(find_leader(&state, 5), Some(ShipKey::Player(2)));
        assert_eq!(
            ships::formation_leader(&state, ShipKey::Player(1)),
            ShipKey::Player(2)
        );
        assert_eq!(allocate_id(&mut state), 1);
        assert_eq!(allocate_id(&mut state), 2);
    }
}
