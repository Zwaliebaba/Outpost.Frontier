//! The conversations of TU25 (`FGcsConversation` and its seven classes,
//! `Content/Gcs.cpp:2178-3371`): what each event says, line by line.
//!
//! A conversation's constructor builds its lines in the C++'s order; a
//! line that cannot play is dropped and, where the C++ clears the
//! conversation's system, makes the conversation invalid
//! ([`Conversation::system`]).

// The x87 comparisons keep their NaN outcome as written (`!(a > b)`).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use super::lines::{self, Line, can_play, rand_pick2};
use super::{
    CombatEvent, DockEvent, Event, LaunchEvent, ScanEvent, find_ship, find_ship_or_solar,
    find_space_obj, is_space_pop,
};
use crate::api;
use crate::content::ships::{self, ShipKey};
use crate::content::{formations, game_support, players};
use crate::events::NpcShipKey;
use crate::server::Cx;

/// The conversation classes, by what `IsValid` tests.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Type 1, `FGcsDockHailConversation`.
    DockHail,
    /// Type 2, `FGcsDockConversation`.
    Dock,
    /// Type 3, `FGcsLaunchConversation`.
    Launch,
    /// Type 5, `FGcsPlayerHailConversation`.
    PlayerHail,
    /// Types 6 to 0x10, `FGcsCombatConversation`.
    Combat,
    /// Types 0x11 to 0x18, `FGcsScanConversation`.
    Scan,
    /// Type 0x19, `FGcsFidgetConversation`.
    Fidget,
}

/// `FGcsConversation` with the fields its classes keep.
#[derive(Clone, Debug, PartialEq)]
pub struct Conversation {
    /// The class.
    pub kind: Kind,
    /// `System` (`+0x18`), `None` once a line could not play.
    pub system: Option<u32>,
    /// The speaker the dock and hail classes test (`+0x1c`).
    pub speaker: u32,
    /// The listener they test (`+0x20`).
    pub listener: u32,
    /// `Lines` (`+0x08`).
    pub lines: Vec<Line>,
    /// `bFinished` (`+0x14`).
    pub finished: bool,
}

impl Conversation {
    fn new(kind: Kind, system: Option<u32>, speaker: u32, listener: u32) -> Self {
        Self {
            kind,
            system,
            speaker,
            listener,
            lines: Vec::new(),
            finished: false,
        }
    }

    /// `AddLine(Line)` (`:2223-2235`): a line that can play is appended.
    fn add(&mut self, cx: &Cx<'_>, line: Line) -> bool {
        if can_play(cx, &line) {
            self.lines.push(line);
            true
        } else {
            false
        }
    }

    /// A line that can play is appended; otherwise the conversation loses
    /// its system (`AddLineOrClear` and the constructors' tails).
    fn add_or_clear(&mut self, cx: &Cx<'_>, line: Line) -> bool {
        if self.add(cx, line) {
            true
        } else {
            self.system = None;
            false
        }
    }
}

/// `IsValid` (slot 2): the dock and dock-hail conversations need their
/// system, the listener's ship (`FindShip`) and the speaker
/// (`FindShipOrSolar`); the others their system.
#[must_use]
pub fn is_valid(cx: &Cx<'_>, c: &Conversation) -> bool {
    match c.kind {
        Kind::DockHail | Kind::Dock => {
            c.system.is_some()
                && find_ship(cx, c.system, c.listener).is_some()
                && find_ship_or_solar(cx, c.system, c.speaker).is_some()
        }
        _ => c.system.is_some(),
    }
}

/// Builds the conversation of `event` (the constructor of its class).
pub fn build(cx: &mut Cx<'_>, event: &Event) -> Conversation {
    match event {
        Event::DockHail {
            system,
            speaker,
            listener,
        } => dock_hail(cx, *system, *speaker, *listener),
        Event::Dock(e) => dock(cx, e),
        Event::Launch(e) => launch(cx, e),
        Event::PlayerHail {
            system,
            player,
            object,
        } => player_hail(cx, *system, *player, *object),
        Event::Combat(e) => combat(cx, e),
        Event::Scan(e) => scan(cx, e),
        Event::Fidget { system, ship, kind } => fidget(cx, *system, *ship, *kind),
    }
}

/// The NPC ship of a space object found by `FindSpaceObj`, as the C++
/// casts it.
fn npc(key: Option<NpcShipKey>) -> Option<ShipKey> {
    key.map(ShipKey::Npc)
}

/// `FGcsLaunchConversation` (`:2241-2261`): the dock clears the ship; a
/// ship that is no player's acknowledges.
fn launch(cx: &mut Cx<'_>, e: &LaunchEvent) -> Conversation {
    let mut c = Conversation::new(Kind::Launch, e.system, e.speaker, e.listener);
    let line = lines::launch_clearance(cx, c.system, e.speaker, e.listener, e.terminal_type);
    if !c.add_or_clear(cx, line) {
        return c;
    }
    if players::find_by_ship(&cx.logic.content.state.players, e.listener).is_some() {
        return c;
    }
    let line = lines::ack(cx, c.system, e.listener, e.speaker);
    c.add_or_clear(cx, line);
    c
}

/// The lines a guided ship adds by its guide type after it named itself
/// (the dock hail's and the player hail's tails): 2 the trader's home and
/// what it trades (or where it goes), 3 the patrol's home, where it goes and
/// its progress, 4 the wanderer's home and where it goes (`with_progress`
/// false: the player hail's 4 says its home only).
fn guided_tail(
    cx: &mut Cx<'_>,
    c: &mut Conversation,
    guide_type: i32,
    speaker: u32,
    listener: u32,
    wander_going_to: bool,
) {
    match guide_type {
        2 => {
            let line = lines::trade_id_home(cx, c.system, speaker, listener);
            if !c.add_or_clear(cx, line) {
                return;
            }
            let status = lines::trade_status(cx, c.system, speaker, listener);
            if c.add(cx, status) {
                return;
            }
            let line = lines::going_to(cx, c.system, speaker, listener);
            c.add_or_clear(cx, line);
        }
        3 => {
            let line = lines::patrol_id_home(cx, c.system, speaker, listener);
            if !c.add_or_clear(cx, line) {
                return;
            }
            let line = lines::going_to(cx, c.system, speaker, listener);
            if !c.add_or_clear(cx, line) {
                return;
            }
            let line = lines::patrol_status(cx, c.system, speaker, listener);
            c.add_or_clear(cx, line);
        }
        4 => {
            let line = lines::patrol_id_home(cx, c.system, speaker, listener);
            if !c.add_or_clear(cx, line) || !wander_going_to {
                return;
            }
            let line = lines::going_to(cx, c.system, speaker, listener);
            c.add_or_clear(cx, line);
        }
        _ => {}
    }
}

/// `FGcsDockHailConversation` (`:3262-3357`): the station's attitude towards
/// the ship (a station without a reputation handle says nothing), its
/// hail; a SpacePop ship answers: angry at or below -0.6, else its data,
/// the station's acknowledgement and what its guide is doing.
fn dock_hail(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Conversation {
    let mut c = Conversation::new(Kind::DockHail, system, speaker, listener);
    let ship_rep = api::space_obj::rep(cx, listener).unwrap_or(0);
    // The station is not null-tested (D935): none says nothing.
    let Some(station_rep) = find_ship_or_solar(cx, system, speaker).and_then(|o| o.reputation(cx))
    else {
        return c;
    };
    let attitude = api::reputation::attitude(cx, station_rep, ship_rep).unwrap_or(0.0);
    let line = lines::dock_hail(cx, c.system, speaker, listener, attitude);
    if !c.add_or_clear(cx, line) {
        return c;
    }
    // The ship is not null-tested (D935): none answers nothing.
    let Some(ship) = npc(find_space_obj(cx, system, listener)) else {
        return c;
    };
    if !is_space_pop(cx, ship) {
        return c;
    }
    if !(attitude > -0.6) {
        let line = lines::hailee_angry(cx, c.system, listener, speaker);
        c.add_or_clear(cx, line);
        return c;
    }
    let line = lines::hailee_data_submit(cx, c.system, listener, speaker);
    if !c.add_or_clear(cx, line) {
        return c;
    }
    let line = lines::dock_hail_received(cx, c.system, speaker, listener);
    if !c.add_or_clear(cx, line) {
        return c;
    }
    let guide_type = super::names::guide_type(cx, Some(ship));
    guided_tail(cx, &mut c, guide_type, listener, speaker, true);
    c
}

/// `FGcsDockConversation` (`:3135-3260`): by the dock target's type, an
/// airlock (`0x400`) nothing; a gate, a hole (`0x40`, `0x800`) or a lane
/// ring (`0x80`) a line of the leader of a SpacePop formation with
/// wingmen (the lane start, or the gate open after a wait, open or
/// delayed); any other target the dock request of a SpacePop ship (its
/// thanks first when it leads wingmen), or the "now cleared" of a ship that
/// waited, then the dock's answer by the response (0 hostile, 2 denied, 3
/// stand by, 4 granted).
fn dock(cx: &mut Cx<'_>, e: &DockEvent) -> Conversation {
    let mut c = Conversation::new(Kind::Dock, e.system, e.speaker, e.listener);
    let target_type = api::space_obj::object_type(cx, e.speaker).unwrap_or(0);
    if target_type == 0x400 {
        return c;
    }
    let object = npc(find_space_obj(cx, e.system, e.listener));
    if matches!(target_type, 0x40 | 0x80 | 0x800) {
        let Some(ship) = object else {
            return c;
        };
        if !is_space_pop(cx, ship) || !super::names::has_formation_wingmen(cx, Some(ship)) {
            return c;
        }
        if ships::is_wingman(&cx.logic.content.state, ship) {
            return c;
        }
        let line = if target_type == 0x80 {
            lines::trade_lane_start(cx, c.system, e.listener, 0)
        } else {
            let (target_system, _) = api::space_obj::jump_target(cx, e.speaker).unwrap_or((0, 0));
            if e.response == 4 {
                if e.waited_in_queue {
                    let line = lines::jump_gate_open_after_delay(cx, c.system, e.listener, 0);
                    if !c.add_or_clear(cx, line) {
                        return c;
                    }
                }
                lines::jump_gate_open(cx, c.system, e.listener, 0, target_system)
            } else {
                lines::jump_gate_delayed(cx, c.system, e.listener, 0, target_system)
            }
        };
        c.add_or_clear(cx, line);
        return c;
    }
    if e.waited_in_queue {
        let line = lines::dock_now_cleared(cx, c.system, e.speaker, e.listener, e.dock_type);
        c.add_or_clear(cx, line);
        return c;
    }
    if let Some(ship) = object.filter(|&s| is_space_pop(cx, s)) {
        let wingmen = super::names::has_formation_wingmen(cx, Some(ship));
        let state = &cx.logic.content.state;
        let leads =
            ships::formation_slot(state, ship) == 0 || ships::formation_id(state, ship) == u32::MAX;
        if wingmen && leads {
            let line = lines::this_our_stop(cx, c.system, e.listener, 0);
            if !c.add_or_clear(cx, line) {
                return c;
            }
        }
        let line = lines::dock_request(cx, c.system, e.listener, e.speaker, e.dock_type);
        if !c.add_or_clear(cx, line) {
            return c;
        }
    }
    let answer = match e.response {
        0 => lines::dock_hostile(cx, c.system, e.speaker, e.listener),
        2 => lines::dock_denied(cx, c.system, e.speaker, e.listener, e.dock_type),
        3 => lines::dock_standby(cx, c.system, e.speaker, e.listener),
        4 => lines::dock_granted(
            cx,
            c.system,
            e.speaker,
            e.listener,
            e.dock_type,
            e.dock_index,
        ),
        _ => return c,
    };
    c.add_or_clear(cx, answer);
    c
}

/// `FGcsPlayerHailConversation` (`:2269-2389`): the object's formation
/// leader answers the player when its attitude towards him is at least
/// -0.6 and it is a SpacePop ship: busy, the combat blow-off; the same
/// player within 15 s, the blow-off; otherwise it names itself and its
/// guide type adds what it is doing.
fn player_hail(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    player: u32,
    object: NpcShipKey,
) -> Conversation {
    let mut c = Conversation::new(Kind::PlayerHail, system, 0, 0);
    let state = &cx.logic.content.state;
    let mut leader = ShipKey::Npc(object);
    let formation = ships::formation_id(state, leader);
    if formation != u32::MAX {
        // `FindFormationLeader` is not null-tested (D935): none answers
        // nothing.
        let Some(l) = formations::find_leader(state, formation) else {
            return c;
        };
        leader = l;
    }
    let leader_rep = ships::reputation(state, leader);
    let player_rep = state.players.get(&player).map_or(-1, |p| p.reputation);
    let player_ship = state
        .players
        .get(&player)
        .and_then(|p| p.ship.as_ref())
        .map_or(0, |s| s.ship);
    let attitude = api::reputation::attitude(cx, leader_rep, player_rep).unwrap_or(0.0);
    if !(attitude >= -0.6) {
        return c;
    }
    if !is_space_pop(cx, leader) {
        return c;
    }
    let leader_id = ships::id(&cx.logic.content.state, leader);
    let busy = ships::base(&cx.logic.content.state, leader).is_some_and(|s| s.in_combat);
    let line = if busy {
        lines::player_hail_combat_blow_off(cx, c.system, leader_id, player_ship)
    } else {
        let now = game_support::game_time(cx);
        let mut blow_off = false;
        if let ShipKey::Npc(npc) = leader
            && let Some(s) = cx.logic.content.state.npc_ships.get_mut(&npc)
        {
            if s.last_hail_player == Some(player) {
                blow_off = game_support::time_distance(s.last_hail_time, now) < 15.0;
            }
            // `SetLastHailPlayer`.
            s.last_hail_time = now;
            s.last_hail_player = Some(player);
        }
        if blow_off {
            lines::player_hail_blow_off(cx, c.system, leader_id, player_ship)
        } else {
            let line = lines::player_hail_identify(cx, c.system, leader_id, player_ship);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            match super::names::guide_type(cx, Some(leader)) {
                2 => {
                    let line = lines::trade_id_home(cx, c.system, leader_id, player_ship);
                    if !c.add_or_clear(cx, line) {
                        return c;
                    }
                    let status = lines::trade_status(cx, c.system, leader_id, player_ship);
                    if c.add(cx, status) {
                        return c;
                    }
                    lines::going_to(cx, c.system, leader_id, player_ship)
                }
                3 => {
                    let line = lines::patrol_id_home(cx, c.system, leader_id, player_ship);
                    if !c.add_or_clear(cx, line) {
                        return c;
                    }
                    let line = lines::going_to(cx, c.system, leader_id, player_ship);
                    if !c.add_or_clear(cx, line) {
                        return c;
                    }
                    lines::patrol_status(cx, c.system, leader_id, player_ship)
                }
                4 => lines::patrol_id_home(cx, c.system, leader_id, player_ship),
                _ => return c,
            }
        }
    };
    c.add_or_clear(cx, line);
    c
}

/// The members of the ship's formation (`CollectFormationMembers`).
fn members(cx: &Cx<'_>, ship: ShipKey) -> Vec<ShipKey> {
    let state = &cx.logic.content.state;
    formations::collect_members(state, ships::formation_id(state, ship))
}

/// The leader's id of a wingman (`GetFormationLeader()->SpaceObjId`).
fn leader_id(cx: &Cx<'_>, ship: ShipKey) -> u32 {
    let state = &cx.logic.content.state;
    ships::id(state, ships::formation_leader(state, ship))
}

/// The members after the first that are no player's.
fn npc_members_after_first(cx: &Cx<'_>, ship: ShipKey) -> Vec<u32> {
    let state = &cx.logic.content.state;
    members(cx, ship)
        .into_iter()
        .skip(1)
        .map(|m| ships::id(state, m))
        .filter(|&id| players::find_by_ship(&state.players, id).is_none())
        .collect()
}

/// `AddFormationAcks(Ship)` (`:2792-2809`): every member after the first
/// that is no player's acknowledges the ship.
fn formation_acks(cx: &mut Cx<'_>, c: &mut Conversation, ship: ShipKey) {
    let ship_id = ships::id(&cx.logic.content.state, ship);
    for member in npc_members_after_first(cx, ship) {
        let line = lines::ack(cx, c.system, member, ship_id);
        if !c.add_or_clear(cx, line) {
            return;
        }
    }
}

/// `AddEnemySightedLine(Ship)` (`:2811-2836`): on `(rand() * 2) / 32768`
/// the first member after the first that is no player's asks to engage
/// (the leader announces the enemy without one), else the leader
/// announces it.
fn enemy_sighted(cx: &mut Cx<'_>, c: &mut Conversation, ship: ShipKey) -> bool {
    let ship_id = ships::id(&cx.logic.content.state, ship);
    let line = if rand_pick2(cx) == 0 {
        lines::combat_enemy_sighted(cx, c.system, ship_id, 0)
    } else {
        match npc_members_after_first(cx, ship).first() {
            Some(&asker) => lines::combat_ask_engage(cx, c.system, asker, 0),
            None => lines::combat_enemy_sighted(cx, c.system, ship_id, 0),
        }
    };
    c.add_or_clear(cx, line)
}

/// `FGcsCombatConversation` (`:2415-2769`), one case per event type.
#[allow(clippy::too_many_lines)]
fn combat(cx: &mut Cx<'_>, e: &CombatEvent) -> Conversation {
    let mut c = Conversation::new(Kind::Combat, e.system, 0, 0);
    let (a, b) = (e.a, e.b);
    let sys = c.system;
    match e.kind {
        6 => {
            let type_b = api::space_obj::object_type(cx, b).unwrap_or(0);
            let type_a = api::space_obj::object_type(cx, a).unwrap_or(0);
            let line = if type_a & 0x7f_0000 != 0 {
                if type_b & 0x7f_0000 == 0 {
                    return c;
                }
                if rand_pick2(cx) != 0 {
                    lines::combat_inflicting_damage(cx, sys, a, 0, e.health)
                } else {
                    lines::combat_taking_damage(cx, sys, b, 0, e.health)
                }
            } else {
                if type_a & 0xfdff == 0 || type_b & 0x7f_0000 == 0 {
                    return c;
                }
                lines::combat_taking_damage(cx, sys, b, 0, e.health)
            };
            c.add_or_clear(cx, line);
        }
        7 => {
            let ship_a = find_ship(cx, sys, a);
            let ship_b = find_ship(cx, sys, b);
            let mut brag = false;
            if let Some(_victim) = ship_b {
                if ship_a.is_some() {
                    match rand_pick2(cx) {
                        1 => brag = true,
                        0 => {}
                        _ => {
                            c.system = None;
                            return c;
                        }
                    }
                }
            } else {
                if ship_a.is_none() {
                    c.system = None;
                    return c;
                }
                brag = true;
            }
            let line = if brag {
                lines::combat_brag(cx, sys, a, 0)
            } else if rand_pick2(cx) != 0 {
                lines::combat_scream(cx, sys, b, 0)
            } else {
                // The victim's leader mourns it; a leading victim screams.
                let state = &cx.logic.content.state;
                let leader =
                    ship_b.map_or(0, |v| ships::id(state, ships::formation_leader(state, v)));
                if leader == b {
                    lines::combat_scream(cx, sys, b, 0)
                } else {
                    lines::combat_death_sympathy(cx, sys, leader)
                }
            };
            c.add_or_clear(cx, line);
        }
        8 => {
            let obj_a = find_space_obj(cx, sys, a);
            let obj_b = find_space_obj(cx, sys, b);
            let line = if obj_a.is_none() {
                lines::combat_in_sights(cx, sys, b, 0)
            } else if obj_b.is_none() || rand_pick2(cx) != 0 {
                lines::combat_has_in_sights(cx, sys, a, 0)
            } else {
                lines::combat_in_sights(cx, sys, b, 0)
            };
            c.add_or_clear(cx, line);
        }
        9 => {
            let Some(ship) = npc(find_space_obj(cx, sys, a)) else {
                if super::names::location_name(cx, a).is_empty() {
                    c.system = None;
                    return c;
                }
                let line = lines::station_engage(cx, sys, a, 0);
                c.add_or_clear(cx, line);
                return c;
            };
            if members(cx, ship).len() <= 1 {
                return c;
            }
            let ship_id = ships::id(&cx.logic.content.state, ship);
            if ships::is_wingman(&cx.logic.content.state, ship) {
                if leader_id(cx, ship).is_multiple_of(5) {
                    return c;
                }
                let line = lines::combat_engaging(cx, sys, ship_id, b);
                c.add_or_clear(cx, line);
                return c;
            }
            if !ship_id.is_multiple_of(5) {
                let line = lines::combat_engaging(cx, sys, ship_id, b);
                c.add_or_clear(cx, line);
                return c;
            }
            if !enemy_sighted(cx, &mut c, ship) {
                return c;
            }
            let line = lines::combat_order_engage(cx, sys, ship_id, 0);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            if rand_pick2(cx) == 0 {
                return c;
            }
            formation_acks(cx, &mut c, ship);
        }
        0xa => {
            let Some(ship) = npc(find_space_obj(cx, sys, a)) else {
                if super::names::location_name(cx, a).is_empty() {
                    c.system = None;
                    return c;
                }
                let line = lines::station_all_clear(cx, sys, a, 0);
                c.add_or_clear(cx, line);
                return c;
            };
            let ship_id = ships::id(&cx.logic.content.state, ship);
            if members(cx, ship).len() <= 1 {
                let line = lines::combat_all_clear(cx, sys, ship_id, 0);
                c.add_or_clear(cx, line);
                return c;
            }
            if ships::is_wingman(&cx.logic.content.state, ship) {
                if leader_id(cx, ship).is_multiple_of(5) {
                    return c;
                }
                let line = if e.reason == 3 {
                    lines::combat_rejoining_form(cx, sys, ship_id, 0)
                } else {
                    lines::combat_all_clear(cx, sys, ship_id, 0)
                };
                c.add_or_clear(cx, line);
                return c;
            }
            if !is_space_pop(cx, ship) || !ship_id.is_multiple_of(5) {
                let line = lines::combat_all_clear(cx, sys, ship_id, 0);
                c.add_or_clear(cx, line);
                return c;
            }
            let line = lines::combat_order_disengage(cx, sys, ship_id, 0);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            let guide_type = super::names::guide_type(cx, Some(ship));
            if guide_type == 2 {
                let line = lines::combat_resume_trade(cx, sys, ship_id, 0);
                if !c.add_or_clear(cx, line) {
                    return c;
                }
            } else if guide_type > 2 && guide_type <= 4 {
                let line = lines::combat_resume_patrol(cx, sys, ship_id, 0);
                if !c.add_or_clear(cx, line) {
                    return c;
                }
            }
            if rand_pick2(cx) == 0 {
                return c;
            }
            formation_acks(cx, &mut c, ship);
        }
        0xb => {
            let Some(ship) = npc(find_space_obj(cx, sys, a)) else {
                return c;
            };
            if members(cx, ship).len() <= 1 {
                return c;
            }
            let ship_id = ships::id(&cx.logic.content.state, ship);
            let orders = if ships::is_wingman(&cx.logic.content.state, ship) {
                if leader_id(cx, ship).is_multiple_of(5) {
                    return c;
                }
                false
            } else {
                ship_id.is_multiple_of(5)
            };
            if !orders {
                let line = lines::combat_enemy_sighted(cx, sys, ship_id, 0);
                if !c.add_or_clear(cx, line) {
                    return c;
                }
                let line = lines::combat_no_engaging(cx, sys, ship_id, 0, e.reason);
                c.add_or_clear(cx, line);
                return c;
            }
            if !enemy_sighted(cx, &mut c, ship) {
                return c;
            }
            let line = lines::combat_order_no_engage(cx, sys, ship_id, 0, e.reason);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            if rand_pick2(cx) == 0 {
                return c;
            }
            formation_acks(cx, &mut c, ship);
        }
        0xc => {
            let Some(ship_a) = npc(find_space_obj(cx, sys, a)) else {
                return c;
            };
            if ships::is_wingman(&cx.logic.content.state, ship_a) {
                return c;
            }
            if find_space_obj(cx, sys, b).is_none() {
                return c;
            }
            if rand_pick2(cx) != 0 {
                let line = lines::combat_offer_help(cx, sys, a, b);
                c.add_or_clear(cx, line);
                return c;
            }
            let line = lines::combat_ask_for_help(cx, sys, b, a);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            let line = lines::combat_coming_in(cx, sys, a, b);
            c.add_or_clear(cx, line);
        }
        0xd => {
            let line = lines::combat_drift(cx, sys, a, 0);
            c.add_or_clear(cx, line);
        }
        0xe => {
            // The object is not null-tested (D935): none says nothing.
            let Some(ship) = npc(find_space_obj(cx, sys, a)) else {
                return c;
            };
            let ship_id = ships::id(&cx.logic.content.state, ship);
            if members(cx, ship).len() <= 1 {
                let line = lines::combat_announce_flee(cx, sys, ship_id, 0, e.reason);
                c.add_or_clear(cx, line);
                return c;
            }
            if ships::is_wingman(&cx.logic.content.state, ship) {
                if leader_id(cx, ship).is_multiple_of(5) {
                    return c;
                }
                let line = lines::combat_announce_flee(cx, sys, ship_id, 0, e.reason);
                c.add_or_clear(cx, line);
                return c;
            }
            if !is_space_pop(cx, ship) || !ship_id.is_multiple_of(5) {
                let line = lines::combat_announce_flee(cx, sys, ship_id, 0, e.reason);
                c.add_or_clear(cx, line);
                return c;
            }
            let line = lines::combat_retreat_towards(cx, sys, ship_id, 0, e.reason);
            if !c.add_or_clear(cx, line) {
                return c;
            }
            if rand_pick2(cx) == 0 {
                return c;
            }
            formation_acks(cx, &mut c, ship);
        }
        0xf => {
            if find_space_obj(cx, sys, b).is_none() {
                return c;
            }
            let line = lines::combat_friendly_fire(cx, sys, b, a);
            c.add_or_clear(cx, line);
        }
        0x10 => {
            let line = lines::combat_taking_lead(cx, sys, a);
            c.add_or_clear(cx, line);
        }
        _ => {}
    }
    c
}

/// `FGcsScanConversation` (`:2838-2914`): one line per type: 0x11 the
/// announcement, 0x12 nothing found, 0x13 the order to drop, 0x14 a player
/// gets the happy line, a scanned NPC complies first; 0x15 a scanned NPC
/// refuses, 0x16 angry, 0x18 the loot taker hears of the destroyed loot.
/// Type 0x17, and 0x15 with a player listener, reach the C++'s tail with
/// no line and call through null (D937): no line, the conversation
/// invalid.
fn scan(cx: &mut Cx<'_>, e: &ScanEvent) -> Conversation {
    let mut c = Conversation::new(Kind::Scan, e.system, e.speaker, e.listener);
    let sys = c.system;
    let (speaker, listener) = (e.speaker, e.listener);
    let is_player = players::find_by_ship(&cx.logic.content.state.players, listener).is_some();
    let line = match e.kind {
        0x11 => Some(lines::scan_announce(cx, sys, speaker, listener)),
        0x12 => Some(lines::scan_single(
            cx,
            sys,
            speaker,
            listener,
            b"gcs_scan_nothingfound",
        )),
        0x13 => Some(lines::scan_single(
            cx,
            sys,
            speaker,
            listener,
            b"gcs_scan_orderdrop",
        )),
        0x14 if is_player => Some(lines::scan_single(
            cx,
            sys,
            speaker,
            listener,
            b"gcs_scan_happy",
        )),
        0x14 => {
            let comply = lines::scan_single(cx, sys, listener, speaker, b"gcs_scan_comply");
            if can_play(cx, &comply) {
                c.lines.push(comply);
                Some(lines::scan_single(
                    cx,
                    sys,
                    speaker,
                    listener,
                    b"gcs_scan_happy",
                ))
            } else {
                Some(comply)
            }
        }
        0x15 if is_player => None,
        0x15 => Some(lines::scan_single(
            cx,
            sys,
            listener,
            speaker,
            b"gcs_scan_refuse",
        )),
        0x16 => Some(lines::scan_single(
            cx,
            sys,
            speaker,
            listener,
            b"gcs_scan_angry",
        )),
        0x18 => Some(lines::scan_single(
            cx,
            sys,
            speaker,
            e.loot_taker,
            b"gcs_scan_lootdestroyed",
        )),
        _ => None,
    };
    match line {
        Some(line) => {
            c.add_or_clear(cx, line);
        }
        None => c.system = None,
    }
    c
}

/// `CollectSystemFormationMembers(&Out, FormationId)` (`:2930-2955`): the
/// system records' space objects (no players) in the formation, sorted by
/// slot (signed) with the VC6 `std::sort`.
fn system_formation_members(cx: &Cx<'_>, formation_id: u32) -> Vec<u32> {
    if formation_id == u32::MAX {
        return Vec::new();
    }
    let state = &cx.logic.content.state;
    let mut out: Vec<(u32, i32)> = Vec::new();
    for record in state.systems.values() {
        for k in &record.space_objs {
            if let Some(s) = state.npc_ships.get(k)
                && s.base.formation_id == formation_id
            {
                out.push((s.base.ship, s.base.formation_slot as i32));
            }
        }
    }
    game_sim::data::vc6::vc6_sort(&mut out, |a, b| a.1 < b.1);
    out.into_iter().map(|(id, _)| id).collect()
}

/// `AddStatusQuery(Members)` (`:3112-3133`): the leader's query, then every
/// member after the first answers it.
fn status_query(cx: &mut Cx<'_>, c: &mut Conversation, ship_id: u32, members: &[u32]) {
    let line = lines::fidget_status_query(cx, c.system, ship_id, 0);
    if !c.add_or_clear(cx, line) {
        return;
    }
    for &member in members.iter().skip(1) {
        let line = lines::fidget_status_report(cx, c.system, member, ship_id);
        if !c.add_or_clear(cx, line) {
            return;
        }
    }
}

/// `FGcsFidgetConversation` (`:2958-3104`): by the ship's guide type (2
/// trade, 3 patrol, 4 wander) and the kind (0 simple, 1 formation status,
/// 2 rumour) one line, or the leader's status query and its wingmen's
/// answers.
fn fidget(cx: &mut Cx<'_>, system: Option<u32>, ship: NpcShipKey, kind: i32) -> Conversation {
    let mut c = Conversation::new(Kind::Fidget, system, 0, 0);
    let sys = c.system;
    let key = ShipKey::Npc(ship);
    let guide_type = super::names::guide_type(cx, Some(key));
    let ship_id = ships::id(&cx.logic.content.state, key);
    let formation = ships::formation_id(&cx.logic.content.state, key);
    let line = match (guide_type, kind) {
        (2, 0) => {
            let (good, _) = super::trade_goods(cx, Some(key));
            if good != 0 && rand_pick2(cx) == 0 {
                lines::fidget_trade_i_hear(cx, sys, ship_id, 0, good)
            } else {
                lines::fidget_single(cx, sys, ship_id, 0, b"gcs_fidget_trade_simple")
            }
        }
        (2 | 4, 1) => {
            let members = system_formation_members(cx, formation);
            if members.len() > 1 {
                status_query(cx, &mut c, ship_id, &members);
                return c;
            }
            let name: &[u8] = if guide_type == 2 {
                b"gcs_fidget_trade_simple"
            } else {
                b"gcs_fidget_patrolsimple"
            };
            lines::fidget_single(cx, sys, ship_id, 0, name)
        }
        (2, 2) => lines::fidget_single(cx, sys, ship_id, 0, b"gcs_fidget_traderumor"),
        (3 | 4, 0) => lines::fidget_single(cx, sys, ship_id, 0, b"gcs_fidget_patrolsimple"),
        (3, 1) => {
            let members = system_formation_members(cx, formation);
            if members.len() <= 1 {
                lines::fidget_single(cx, sys, ship_id, 0, b"gcs_fidget_patrolsimple")
            } else if rand_pick2(cx) != 0 {
                status_query(cx, &mut c, ship_id, &members);
                return c;
            } else {
                let wingman = members[1];
                let line = lines::fidget_waypoint_query(cx, sys, wingman, ship_id);
                if !c.add_or_clear(cx, line) {
                    return c;
                }
                lines::fidget_patrol_status(cx, sys, ship_id, wingman)
            }
        }
        (3 | 4, 2) => lines::fidget_single(cx, sys, ship_id, 0, b"gcs_fidget_patrolrumor"),
        _ => return c,
    };
    c.add_or_clear(cx, line);
    c
}
