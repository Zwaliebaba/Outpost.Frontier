//! The `[MetaBehavior]` guides (`Content/GotoGuides.cpp`): the goto guide
//! (type 5, `FGotoGuide`) leads a target (a mission's ship or the player)
//! along the NN path to a destination with `Guide` directives, waits for
//! it at the lanes, nags it to get going, and fights off what attacks
//! either; the dock guide (type 6, `FDockGuide`) is a goto guide to its
//! dock target that docks there at the end.
//!
//! The target is found by its mission name (`FSystemInfo::
//! FindObjectIdByName` with the ship's controller, [`systems::
//! find_object_id_by_name`]). The voice lines go through the system's
//! comms (`PickVoiceLine` with the suffix `-`, then `SendComm`).

// The C++'s `!(x < y)` tests send NaN to the failing side; the port spells
// them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::ai::content::FleeAnswer;
use neuron_core::id::create_id;
use neuron_core::math::Vector;

use super::super::directives::{self, DirectiveList, DirectiveRef};
use super::super::mission::meta_behavior::MbGuide;
use super::super::mission::obj_list::{DirectiveKind, Dock, Guide as GuideDirective};
use super::super::npc_ships;
use super::super::players;
use super::super::ships::{self, ShipKey};
use super::super::space_paths::{self, NnPathLeg, fast_length, find_entry_ring};
use super::super::systems;
use super::{build_path_directives, ship_position};
use crate::api;
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

/// The zero leg (`FNnPathLeg Empty = {}`): kind 0.
fn zero_leg() -> NnPathLeg {
    NnPathLeg {
        kind: 0,
        point_a: Vector::ZERO,
        point_b: Vector::ZERO,
        object_a: 0,
        object_b: 0,
    }
}

/// `FGotoGuide` (type 5, 0x1b0 bytes), its line guide part (`FLineGuide`)
/// included.
#[derive(Clone, Debug, PartialEq)]
pub struct GotoGuide {
    /// `Line` (`+0x14`): the description's line.
    pub line: Vec<u8>,
    /// `FirstPosition` (`+0x118`): the follow offset of its guide
    /// directives.
    pub first_position: Vector,
    /// `Destination` (`+0x124`).
    pub destination: Vector,
    /// `Range` (`+0x130`).
    pub range: f32,
    /// `TargetName` (`+0x134`).
    pub target_name: Vec<u8>,
    /// `TargetId` (`+0x168`).
    pub target_id: u32,
    /// `bCruise` (`+0x16c`).
    pub cruise: u8,
    /// `bNoCruise` (`+0x16d`).
    pub no_cruise: u8,
    /// `+0x16e` (1): the guide directives' `Field_A0`.
    pub field_0x16e: u8,
    /// `Directive` (`+0x170`): the last directive handed out, one
    /// reference held.
    pub directive: Option<DirectiveRef>,
    /// `State` (`+0x174`): 1 to 10 (the dock guide 12 and 13 too).
    pub state: i32,
    /// `PrevState` (`+0x178`).
    pub prev_state: i32,
    /// `Priority` (`+0x17c`): the guide directives' priority.
    pub priority: i32,
    /// `Path` (`+0x180`).
    pub path: Vec<NnPathLeg>,
    /// `Waypoint` (`+0x18c`; the C++ leaves it unset, 0 here).
    pub waypoint: Vector,
    /// `Timer` (`+0x198`).
    pub timer: f32,
    /// `NagCount` (`+0x19c`).
    pub nag_count: i32,
    /// `CombatIds` (`+0x1a0`).
    pub combat_ids: Vec<u32>,
    /// `bRestart` (`+0x1ac`).
    pub restart: bool,
    /// `bNodeEndSent` (`+0x1ad`).
    pub node_end_sent: bool,
}

/// `FDockGuide`'s own members (`+0x1b0` to `+0x1b5`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DockPart {
    /// `DockTargetId` (`+0x1b0`).
    pub dock_target: u32,
    /// `bDockRestart` (`+0x1b4`).
    pub dock_restart: bool,
    /// `bDocking` (`+0x1b5`): the goto part is over.
    pub docking: bool,
}

/// The ship's system record id and controller.
fn system_and_controller(cx: &Cx<'_>, ship: NpcShipKey) -> (u32, u32) {
    npc_ships::get(cx, ship).map_or((0, 0), |s| (s.base.system.unwrap_or(0), s.controller))
}

/// `Ship->GetSystem()` is a record.
fn has_record(cx: &Cx<'_>, ship: NpcShipKey) -> bool {
    let (system, _) = system_and_controller(cx, ship);
    cx.logic.content.state.systems.contains_key(&system)
}

/// The NN path from the ship to `to` (`BuildNnPath(System, &Ship, &To,
/// &Path, 0x19, &1000, 0)`).
fn nn_path(cx: &Cx<'_>, ship: NpcShipKey, to: Vector) -> Vec<NnPathLeg> {
    let (system, _) = system_and_controller(cx, ship);
    let from = ship_position(cx, ship);
    cx.logic
        .content
        .state
        .systems
        .get(&system)
        .map(|record| space_paths::build_nn_path(record, from, to, 1000.0).1)
        .unwrap_or_default()
}

/// `(z² + y²) + x²` of `a - b`, the differences stored as floats.
fn distance_squared_zyx(a: Vector, b: Vector) -> f64 {
    let x = a.x - b.x;
    let y = a.y - b.y;
    let z = a.z - b.z;
    (f64::from(z) * f64::from(z) + f64::from(y) * f64::from(y)) + f64::from(x) * f64::from(x)
}

impl GotoGuide {
    /// `FGotoGuide(Desc)` (`GotoGuides.cpp:96-123`): the description's
    /// members, state 1; with a ship its target found by name, the NN path
    /// to the destination (range 1000) when there is none, the nag timer
    /// at 0 and the ship among the guided ships.
    pub(super) fn new(cx: &mut Cx<'_>, desc: &MbGuide, ship: Option<NpcShipKey>) -> Self {
        let mut g = Self {
            line: desc.line.clone(),
            first_position: desc.first_position,
            destination: desc.destination,
            range: desc.range,
            target_name: desc.target_name.clone(),
            target_id: 0,
            cruise: desc.field_0x174,
            no_cruise: desc.field_0x175,
            field_0x16e: desc.field_0x176,
            directive: None,
            state: 1,
            prev_state: 0,
            priority: desc.priority,
            // `FMbGotoGuide`'s legs: nothing writes them (P3.5a2).
            path: Vec::new(),
            waypoint: Vector::ZERO,
            timer: 0.0,
            nag_count: 0,
            combat_ids: Vec::new(),
            restart: false,
            node_end_sent: false,
        };
        let Some(ship) = ship else {
            return g;
        };
        g.target_id = find_target(cx, ship, &g.target_name);
        if has_record(cx, ship) {
            g.target_id = find_target(cx, ship, &g.target_name);
            if g.path.is_empty() {
                g.path = nn_path(cx, ship, g.destination);
            }
        }
        g.timer = 0.0;
        g.nag_count = 0;
        super::super::space_support::register_guided_ship(cx, ship);
        g
    }

    /// `SetState(New)`: the state changed (the old kept) and a restart
    /// asked, unless it is the state already.
    fn set_state(&mut self, state: i32) {
        if self.state == state {
            return;
        }
        self.prev_state = self.state;
        self.state = state;
        self.restart = true;
    }

    /// `GetFirstLeg`: the path's first leg, the zero leg for an empty path.
    fn first_leg(&self) -> NnPathLeg {
        self.path.first().copied().unwrap_or_else(zero_leg)
    }

    /// `Reset` (`:189-193`): state 1, previous 0.
    pub(super) fn reset(&mut self) {
        self.state = 1;
        self.prev_state = 0;
    }
}

/// `FindObjectIdByName(&TargetName, ControllerId)` on the ship's system.
fn find_target(cx: &Cx<'_>, ship: NpcShipKey, name: &[u8]) -> u32 {
    let (system, controller) = system_and_controller(cx, ship);
    systems::find_object_id_by_name(cx, system, name, controller)
}

/// `RefreshTarget` (`:224-229`; the dock guide's `:771-778`): the target
/// found again; the dock guide's destination is its dock target's position
/// again.
pub(super) fn refresh_target(
    cx: &Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut GotoGuide,
    dock: Option<&DockPart>,
) {
    if let Some(ship) = ship {
        g.target_id = find_target(cx, ship, &g.target_name);
    }
    if let Some(d) = dock
        && let Ok((p, _)) = api::space_obj::location(cx, d.dock_target)
    {
        g.destination = p;
    }
}

/// `FDockGuide(Desc)` (`:718-737`): the goto guide, then the dock target;
/// the target found again with a ship, the destination the dock target's
/// position, and the path rebuilt to it in a system.
pub(super) fn new_dock(
    cx: &mut Cx<'_>,
    desc: &MbGuide,
    dock_target: u32,
    ship: Option<NpcShipKey>,
) -> (GotoGuide, DockPart) {
    let mut g = GotoGuide::new(cx, desc, ship);
    let dock = DockPart {
        dock_target,
        dock_restart: false,
        docking: false,
    };
    if let Some(ship) = ship {
        g.target_id = find_target(cx, ship, &g.target_name);
    }
    if let Ok((p, _)) = api::space_obj::location(cx, dock_target) {
        g.destination = p;
    }
    if let Some(ship) = ship
        && has_record(cx, ship)
    {
        g.path = nn_path(cx, ship, g.destination);
    }
    (g, dock)
}

/// The copy `Clone` makes (`:127-150`, the dock guide's `:741-752`): the
/// held directive `AddRef`'ed; a dock guide's `bDocking` false, its target
/// found again with a ship and its destination its dock target's position.
pub(super) fn copy(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &GotoGuide,
    dock: Option<&DockPart>,
) -> (GotoGuide, Option<DockPart>) {
    if let Some(d) = g.directive {
        cx.logic.content.state.directives.add_ref(d);
    }
    let mut copy = g.clone();
    let dock = dock.map(|d| DockPart {
        docking: false,
        ..d.clone()
    });
    if let Some(d) = &dock {
        refresh_target(cx, ship, &mut copy, Some(d));
    }
    (copy, dock)
}

/// `~FGotoGuide` (`:156-160`): the target's manoeuvres unlocked, the ship
/// off the guided ships, the held directive released.
pub(super) fn destroy(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, g: &GotoGuide) {
    let _ = api::ai::lock_maneuvers(cx, g.target_id, false);
    if let Some(ship) = ship {
        super::super::space_support::unregister_guided_ship(cx, ship);
    }
    if let Some(d) = g.directive {
        cx.logic.content.state.directives.release(d);
    }
}

/// `FGotoGuide::SetShip` (`:203-221`): the guided ship replaced; a guide
/// that had none finds its target (`RefreshTarget`); in a system the path
/// to the destination is rebuilt; the ship joins the guided ships.
pub(super) fn set_ship(
    cx: &mut Cx<'_>,
    old: Option<NpcShipKey>,
    ship: Option<NpcShipKey>,
    g: &mut GotoGuide,
    dock: Option<&DockPart>,
) {
    if let Some(old) = old {
        super::super::space_support::unregister_guided_ship(cx, old);
    }
    let Some(key) = ship else {
        return;
    };
    if old.is_none() {
        refresh_target(cx, ship, g, dock);
    }
    if has_record(cx, key) {
        g.path = nn_path(cx, key, g.destination);
    }
    super::super::space_support::register_guided_ship(cx, key);
}

/// `FGotoGuide::RequestFleeDestination` (`:234-313`): on the path's first
/// leg (copied once) by the previous state: 2 the leg's start unless the
/// ship is within 1500 of it (then state 6), 3 and 5 the leg's end, 6 the
/// leg's end unless within 1500 (then state 9), 9 the leg dropped (state 2,
/// or 10 when the path is empty); each a point (type 3) within 1500. The
/// walk goes on while nothing is found and the path is not empty. A
/// previous state outside those five, or a state change that changes
/// nothing, repeats without end in the C++; the port answers none there
/// (D863).
pub(super) fn flee(cx: &Cx<'_>, ship: Option<NpcShipKey>, g: &mut GotoGuide) -> Option<FleeAnswer> {
    let ship = ship?;
    let leg = *g.path.first()?;
    let mut answer = FleeAnswer {
        kind: 3,
        radius: 1500.0,
        ..FleeAnswer::default()
    };
    loop {
        let before = (g.state, g.prev_state, g.path.len());
        match g.prev_state {
            2 => {
                let position = ship_position(cx, ship);
                if !(fast_length(distance_squared_zyx(position, leg.point_a)) < 1500.0) {
                    answer.position = leg.point_a;
                    return Some(answer);
                }
                g.set_state(6);
            }
            3 | 5 => {
                answer.position = leg.point_b;
                return Some(answer);
            }
            6 => {
                let position = ship_position(cx, ship);
                if !(fast_length(distance_squared_zyx(position, leg.point_b)) < 1500.0) {
                    answer.position = leg.point_b;
                    return Some(answer);
                }
                g.set_state(9);
            }
            9 => {
                g.path.remove(0);
                if g.path.is_empty() {
                    g.set_state(10);
                } else {
                    g.set_state(2);
                }
            }
            _ => {}
        }
        if g.path.is_empty() {
            return None;
        }
        if (g.state, g.prev_state, g.path.len()) == before {
            return None;
        }
    }
}

/// The ship's formation and the target's: the target is a ship of the
/// ship's system in the ship's formation (both outside one count).
fn target_in_ship_group(cx: &Cx<'_>, ship: NpcShipKey, target: u32) -> bool {
    let (system, _) = system_and_controller(cx, ship);
    if !cx.logic.content.state.systems.contains_key(&system) {
        return false;
    }
    let Some(found) = ships::find_in_system(cx, system, target) else {
        return false;
    };
    let state = &cx.logic.content.state;
    ships::formation_id(state, found) == ships::formation_id(state, ShipKey::Npc(ship))
}

/// `IsTargetOnPath(Kind, ObjectId)` (`:317-339`): kind 7 (a lane) is on
/// the path when the first leg is a lane and the object's lane has one of
/// its two rings; any other kind tests the target's formation.
fn is_target_on_path(cx: &Cx<'_>, ship: NpcShipKey, g: &GotoGuide, kind: i32, object: u32) -> bool {
    if kind != 7 {
        return target_in_ship_group(cx, ship, g.target_id);
    }
    let leg = g.first_leg();
    if leg.kind != 0 {
        return false;
    }
    let (system, _) = system_and_controller(cx, ship);
    let Some(rings) = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.lane_rings_of_ring(object))
    else {
        return false;
    };
    rings
        .iter()
        .any(|r| r.object == leg.object_a || r.object == leg.object_b)
}

/// `FindLaneEntryNearTarget` (`:343-364`): on a lane first leg, the entry
/// ring of that lane nearest the target becomes the waypoint when it lies
/// within 3000 of the target.
fn find_lane_entry_near_target(cx: &Cx<'_>, ship: NpcShipKey, g: &mut GotoGuide) -> bool {
    let leg = g.first_leg();
    if leg.kind != 0 {
        return false;
    }
    let target = api::space_obj::location(cx, g.target_id).map_or(Vector::ZERO, |(p, _)| p);
    let (system, _) = system_and_controller(cx, ship);
    let Some(rings) = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.lane_rings_of_ring(leg.object_a))
    else {
        return false;
    };
    let entry = find_entry_ring(target, rings, None);
    let dx = target.x - entry.position.x;
    let dy = target.y - entry.position.y;
    let dz = target.z - entry.position.z;
    let length_squared = (f64::from(dz) * f64::from(dz) + f64::from(dy) * f64::from(dy))
        + f64::from(dx) * f64::from(dx);
    if !(3000.0 * 3000.0 > length_squared) {
        return false;
    }
    g.waypoint = entry.position;
    true
}

/// `FindLegPointNearTarget` (`:368-397`): the first leg point (each leg's
/// start but the first leg's, then its end) within 1500 of the target
/// becomes the waypoint.
fn find_leg_point_near_target(cx: &Cx<'_>, ship: NpcShipKey, g: &mut GotoGuide) -> bool {
    if g.path.is_empty() {
        return false;
    }
    let target = api::space_obj::location(cx, g.target_id).map_or(Vector::ZERO, |(p, _)| p);
    let limit = 1500.0 * 1500.0;
    for (index, leg) in g.path.iter().enumerate() {
        if index != 0 && limit > distance_squared_zyx(target, leg.point_a) {
            let position = ship_position(cx, ship);
            if distance_squared_zyx(position, target) >= 0.0 {
                g.waypoint = leg.point_a;
                return true;
            }
        }
        if limit > distance_squared_zyx(target, leg.point_b) {
            let position = ship_position(cx, ship);
            if distance_squared_zyx(position, target) >= 0.0 {
                g.waypoint = leg.point_b;
                return true;
            }
        }
    }
    false
}

/// The guide's voice line (`:44-60`): the ship's voice, then in a system
/// `PickVoiceLine(Name, "-")` and on success a comm line to the target
/// (0.5 s).
fn send_guide_comm(cx: &mut Cx<'_>, ship: NpcShipKey, target: u32, name: &[u8]) {
    let id = npc_ships::space_obj(cx, ship);
    let voice = api::space_obj::voice_id(cx, id).unwrap_or(0);
    if !has_record(cx, ship) {
        return;
    }
    let Some(line) = npc_ships::pick_voice_line(cx, name, b"-", voice) else {
        return;
    };
    let text = api::misc::nickname_id(&line);
    npc_ships::send_comm(cx, id, target, text);
}

/// A combat id listed once.
fn add_combat_id(g: &mut GotoGuide, id: u32) {
    if !g.combat_ids.contains(&id) {
        g.combat_ids.push(id);
    }
}

/// A combat id gone; with none left the ship is no longer muted and the
/// state goes back to the one before the fight.
fn drop_combat_id(cx: &mut Cx<'_>, ship: NpcShipKey, g: &mut GotoGuide, id: u32) {
    if let Some(at) = g.combat_ids.iter().position(|&c| c == id) {
        g.combat_ids.remove(at);
    }
    if g.combat_ids.is_empty() {
        npc_ships::set_muted(cx, ship, false);
        if g.prev_state != g.state {
            std::mem::swap(&mut g.state, &mut g.prev_state);
            g.restart = true;
        }
    }
}

/// `FGotoGuide::HandleEvent` (`:401-541`) without its restart: the lane,
/// formation, destruction, combat and heartbeat messages move the state.
/// Its caller ends with `Start` when a restart was asked (`FinishEvent`).
pub(super) fn handle_event(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut GotoGuide,
    done: &mut bool,
    message: &ControllerMessage<'_>,
) {
    let approach_range = if 1500.0 < g.range { g.range } else { 1500.0 };
    let Some(key) = ship else {
        return;
    };
    let mut leg = NnPathLeg {
        kind: 1,
        ..zero_leg()
    };
    if let Some(first) = g.path.first() {
        leg = *first;
    } else {
        g.set_state(10);
    }
    let approach = f64::from(approach_range) * f64::from(approach_range);
    let own = npc_ships::space_obj(cx, key);
    match message {
        ControllerMessage::TradelaneExit { ship: id, .. } => {
            if *id == g.target_id {
                let _ = api::ai::lock_maneuvers(cx, g.target_id, false);
            }
            if *id == own
                && g.state == 7
                && approach < distance_squared_zyx(ship_position(cx, key), leg.point_b)
            {
                g.set_state(3);
            }
        }
        ControllerMessage::TradelaneEnter {
            ship: id, entrance, ..
        } => {
            if *id == g.target_id && is_target_on_path(cx, key, g, 7, *entrance) {
                let _ = api::ai::lock_maneuvers(cx, g.target_id, true);
            }
        }
        ControllerMessage::TradelaneRequest { ship: id, ring, .. } => {
            if (2..=5).contains(&g.state)
                && *id == g.target_id
                && is_target_on_path(cx, key, g, 7, *ring)
            {
                g.set_state(7);
            }
        }
        ControllerMessage::FormationRequest {
            ship: id, leader, ..
        } => {
            if g.state == 3 && g.target_id == *id && own == *leader {
                g.prev_state = 3;
                g.state = 7;
                g.restart = true;
            }
        }
        ControllerMessage::ObjectDestroyed(event) => {
            if g.state == 4 {
                drop_combat_id(cx, key, g, event.id);
            }
        }
        ControllerMessage::CombatState {
            state: kind,
            param1: a,
            param2: b,
            ..
        } => {
            let (kind, a, b) = (*kind, *a, *b);
            let involved = a == g.target_id || a == own || b == g.target_id || b == own;
            if g.state == 4 {
                if kind == 4 || kind == 3 {
                    drop_combat_id(cx, key, g, a);
                } else if kind == 0 && involved {
                    add_combat_id(g, a);
                }
            } else if matches!(g.state, 2 | 3 | 5 | 7) && kind == 0 && involved {
                add_combat_id(g, a);
                if g.state == 7 {
                    g.state = 3;
                }
                npc_ships::set_muted(cx, key, true);
                g.set_state(4);
            }
        }
        ControllerMessage::Heartbeat { elapsed } => {
            handle_timer(cx, key, g, done, *elapsed, &leg, approach);
        }
        _ => {}
    }
}

/// The heartbeat (`HandleTimer`, `:551-697`) by state; state 10 ends the
/// guide (`bDone`).
fn handle_timer(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    g: &mut GotoGuide,
    done: &mut bool,
    dt: f32,
    leg: &NnPathLeg,
    approach: f64,
) {
    let position = || ship_position(cx, ship);
    match g.state {
        1 => g.set_state(2),
        2 => {
            let p = position();
            let d = Vector::new(
                (f64::from(p.x) - f64::from(leg.point_a.x)) as f32,
                (f64::from(p.y) - f64::from(leg.point_a.y)) as f32,
                (f64::from(p.z) - f64::from(leg.point_a.z)) as f32,
            );
            let distance = (f64::from(d.x) * f64::from(d.x) + f64::from(d.z) * f64::from(d.z))
                + f64::from(d.y) * f64::from(d.y);
            if !(approach > distance) {
                if find_leg_point_near_target(cx, ship, g) {
                    g.set_state(8);
                }
                return;
            }
            if leg.kind == 0 {
                let Some(player) =
                    players::find_by_ship(&cx.logic.content.state.players, g.target_id)
                else {
                    return;
                };
                let (e4, e8) = cx
                    .logic
                    .content
                    .state
                    .players
                    .get(&player)
                    .map_or((0, 0), |p| (p.value_e4, p.value_e8));
                if is_target_on_path(cx, ship, g, e4 as i32, e8) {
                    g.set_state(7);
                    return;
                }
                let target = g.target_id;
                send_guide_comm(cx, ship, target, b"mb_gg_cmd_enter_tl");
                g.set_state(3);
            } else if leg.kind == 1 {
                g.set_state(6);
            }
        }
        3 => {
            let near = find_lane_entry_near_target(cx, ship, g)
                || target_in_ship_group(cx, ship, g.target_id);
            let large = ships::is_in_ship_class(cx, ShipKey::Npc(ship), create_id(b"LARGE_SHIPS"));
            if near && !large {
                g.timer = 0.0;
                g.nag_count = 0;
                g.set_state(5);
            } else if find_leg_point_near_target(cx, ship, g) {
                g.timer = 0.0;
                g.nag_count = 0;
                g.set_state(8);
            } else if !(g.timer < 30.0) {
                if g.nag_count >= 5 {
                    if leg.kind == 0 {
                        g.set_state(7);
                    } else if leg.kind == 1 {
                        g.set_state(6);
                    }
                } else {
                    let target = g.target_id;
                    send_guide_comm(cx, ship, target, b"mb_gg_cmd_get_going");
                    g.nag_count += 1;
                }
                g.timer = 0.0;
            }
            g.timer = (f64::from(g.timer) + f64::from(dt)) as f32;
        }
        5 => {
            let player = players::find_by_ship(&cx.logic.content.state.players, g.target_id);
            if !find_lane_entry_near_target(cx, ship, g)
                && !target_in_ship_group(cx, ship, g.target_id)
            {
                g.set_state(3);
                return;
            }
            if let Some(player) = player {
                let (e4, e8) = cx
                    .logic
                    .content
                    .state
                    .players
                    .get(&player)
                    .map_or((0, 0), |p| (p.value_e4, p.value_e8));
                if is_target_on_path(cx, ship, g, e4 as i32, e8) {
                    g.set_state(7);
                    return;
                }
            }
            if approach > distance_squared_zyx(position(), leg.point_b) {
                g.set_state(9);
            }
        }
        6 | 7 => {
            if approach > distance_squared_zyx(position(), leg.point_b) {
                g.set_state(9);
            }
        }
        8 => {
            if approach > distance_squared_zyx(position(), g.waypoint) {
                if has_record(cx, ship) {
                    g.path = nn_path(cx, ship, g.destination);
                }
                g.set_state(2);
            }
        }
        9 => {
            let (system, _) = system_and_controller(cx, ship);
            let target = ships::find_in_system(cx, system, g.target_id);
            if find_leg_point_near_target(cx, ship, g) {
                g.node_end_sent = false;
                g.set_state(8);
                return;
            }
            if let Some(t) = target {
                let own = position();
                let other = ships::position(cx, t);
                if distance_squared_zyx(other, own) < 0.0 {
                    g.node_end_sent = false;
                    g.path.remove(0);
                    if g.path.is_empty() {
                        g.set_state(10);
                    } else {
                        g.set_state(2);
                    }
                    return;
                }
            }
            if g.node_end_sent {
                return;
            }
            g.node_end_sent = true;
            let target = g.target_id;
            send_guide_comm(cx, ship, target, b"mb_gg_cmd_at_node_end");
        }
        10 => *done = true,
        _ => {}
    }
}

/// `SetGuide(&TargetId, &FollowOffset, &Destination, Field50, bNoCruise,
/// bCruise, FieldA0, Field58)` (`MissionObjList.cpp:702-722`): a guide
/// directive's goto and follow parts.
#[allow(clippy::too_many_arguments)]
fn set_guide(
    d: &mut GuideDirective,
    target: u32,
    follow_offset: Vector,
    destination: Vector,
    field_50: f32,
    no_cruise: bool,
    cruise: bool,
    field_a0: bool,
    field_58: bool,
) {
    let goto = &mut d.op.goto;
    goto.kind = 1;
    goto.destination = destination;
    goto.field_58 = u8::from(field_58);
    goto.range = field_50;
    goto.field_5c = target;
    goto.no_cruise = u8::from(no_cruise);
    goto.cruise = u8::from(cruise);
    goto.field_60 = 200.0;
    goto.field_64 = 500.0;
    d.op.follow.target = target;
    d.op.follow.offset = follow_offset;
    d.op.field_a0 = u8::from(field_a0);
}

/// `KeepAndQueue(Guide, New, List)`: the new directive held by the guide
/// (the old one released) and appended to the list (one reference each;
/// the creating local's is released).
fn keep_and_queue(
    cx: &mut Cx<'_>,
    g: &mut GotoGuide,
    kind: DirectiveKind,
    list: &mut DirectiveList,
) {
    let arena = &mut cx.logic.content.state.directives;
    let new = arena.create(kind);
    arena.add_ref(new);
    if let Some(old) = g.directive.replace(new) {
        arena.release(old);
    }
    arena.add_ref(new);
    list.push(new);
    arena.release(new);
}

/// A guide directive of the goto guide to `destination`.
fn guide_directive(
    g: &GotoGuide,
    destination: Vector,
    range: f32,
    field_58: bool,
    priority: bool,
) -> DirectiveKind {
    let mut d = GuideDirective::default();
    set_guide(
        &mut d,
        g.target_id,
        g.first_position,
        destination,
        range,
        g.no_cruise != 0,
        g.cruise != 0,
        g.field_0x16e != 0,
        field_58,
    );
    if priority {
        d.priority = g.priority as u32;
    }
    DirectiveKind::Guide(d)
}

/// `FGotoGuide::Start` (`:703-797`): only after a restart was asked and
/// while the ship is not in a lane; the state's directives (possibly none)
/// replace the ship's (priority 0): 2 a guide to the first leg's start, 3
/// none (the ship's directives cancelled), 5 a guide to the waypoint
/// (without the guide's priority), 6 a guide to the leg's end, 7 a dock
/// at the lane's entry ring nearest the ship toward the leg's end ring, 8
/// the path to the waypoint, 10 a guide to the destination within the
/// guide's range.
pub(super) fn start(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, g: &mut GotoGuide) {
    let leg = g.first_leg();
    if !g.restart {
        return;
    }
    let Some(key) = ship else {
        return;
    };
    if ships::base(&cx.logic.content.state, ShipKey::Npc(key)).is_some_and(|b| b.lane_entry != 0) {
        return;
    }
    g.restart = false;
    let mut list = DirectiveList::new();
    match g.state {
        2 => {
            let kind = guide_directive(g, leg.point_a, 500.0, false, true);
            keep_and_queue(cx, g, kind, &mut list);
        }
        3 => npc_ships::cancel_directives(cx, ShipKey::Npc(key)),
        5 => {
            let kind = guide_directive(g, g.waypoint, 500.0, false, false);
            keep_and_queue(cx, g, kind, &mut list);
        }
        6 => {
            let kind = guide_directive(g, leg.point_b, 500.0, true, true);
            keep_and_queue(cx, g, kind, &mut list);
        }
        7 => {
            if has_record(cx, key) {
                let (system, _) = system_and_controller(cx, key);
                let rings = cx
                    .logic
                    .content
                    .state
                    .systems
                    .get(&system)
                    .and_then(|r| r.lane_rings_of_ring(leg.object_b))
                    .map(<[_]>::to_vec)
                    .unwrap_or_default();
                let entry = find_entry_ring(ship_position(cx, key), &rings, None);
                let mut dock = Dock::default();
                dock.op.dock_object = entry.object;
                dock.op.field_10 = leg.object_b;
                keep_and_queue(cx, g, DirectiveKind::Dock(dock), &mut list);
            }
        }
        8 => {
            if has_record(cx, key) {
                let (system, _) = system_and_controller(cx, key);
                let from = ship_position(cx, key);
                list = build_path_directives(cx, system, from, g.waypoint, g.range);
            }
        }
        10 => {
            let kind = guide_directive(g, g.destination, g.range, false, true);
            keep_and_queue(cx, g, kind, &mut list);
        }
        _ => {}
    }
    npc_ships::set_directives(cx, ShipKey::Npc(key), &list, 0);
    directives::clear_list(&mut cx.logic.content.state.directives, &mut list);
}

/// `FDockGuide::HandleEvent` (`:782-871`): the heartbeat in state 10 turns
/// to docking (state 12) and in state 13 ends the guide; in state 12 the
/// ship's arrival (`Msg0x27`: the dock target's base for a station, the
/// target itself for a ring), its `DockComplete` at a gate or hole (type
/// `0xc40`) or its entry into the lane at the dock target (a ring) is
/// state 13. The other messages, and these outside their states, go to the
/// goto guide unless the docking has begun. True when a dock restart is
/// asked.
pub(super) fn dock_handle_event(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut GotoGuide,
    dock: &mut DockPart,
    done: &mut bool,
    message: &ControllerMessage<'_>,
) -> bool {
    let own = ship.map_or(0, |s| npc_ships::space_obj(cx, s));
    let mut forward = false;
    // State 13 unless the guide is there already.
    let arrive = |g: &mut GotoGuide, dock: &mut DockPart| {
        if g.state != 13 {
            g.prev_state = g.state;
            g.state = 13;
            dock.dock_restart = true;
        }
    };
    match message {
        ControllerMessage::Heartbeat { .. } => {
            if g.state == 10 {
                dock.docking = true;
                *done = false;
                g.prev_state = 10;
                g.state = 12;
                dock.dock_restart = true;
            } else if g.state == 13 {
                *done = true;
            } else {
                forward = true;
            }
        }
        ControllerMessage::Msg0x27 {
            ship: id,
            base,
            target,
            ..
        } => {
            if g.state != 12 {
                forward = true;
            } else if *id == own
                && let Ok(kind) = api::space_obj::object_type(cx, dock.dock_target)
            {
                let docking_target =
                    api::space_obj::docking_target(cx, dock.dock_target).unwrap_or(0);
                let arrived =
                    (kind & 0x100 != 0 && *base & 0x3fff_ffff != 0 && docking_target == *base)
                        || (kind & 0x20 != 0 && dock.dock_target == *target);
                if arrived {
                    arrive(g, dock);
                }
            }
        }
        ControllerMessage::DockComplete {
            ship: id, dock: at, ..
        } => {
            if g.state != 12 {
                forward = true;
            } else if *id == own
                && let Ok(kind) = api::space_obj::object_type(cx, dock.dock_target)
                && kind & 0xc40 != 0
                && dock.dock_target == *at
            {
                arrive(g, dock);
            }
        }
        ControllerMessage::TradelaneEnter {
            ship: id, entrance, ..
        } => {
            if g.state != 12 {
                forward = true;
            } else if *id == own && *entrance == dock.dock_target {
                g.prev_state = 12;
                g.state = 13;
                dock.dock_restart = true;
            }
        }
        _ => forward = true,
    }
    if forward && !dock.docking {
        handle_event(cx, ship, g, done, message);
        // The goto guide's `FinishEvent`: its `Start` is the dock guide's.
        if g.restart {
            dock_start(cx, ship, g, dock);
        }
    }
    dock.dock_restart
}

/// `FDockGuide::Start` (`:877-897`): the goto guide's `Start` until the
/// docking begins; then, after a dock restart and while the ship is not in
/// a lane, state 12 docks at the dock target (`SetTarget(&Target, &0)`).
pub(super) fn dock_start(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut GotoGuide,
    dock: &mut DockPart,
) {
    if !dock.docking {
        start(cx, ship, g);
    }
    if !dock.dock_restart {
        return;
    }
    let Some(key) = ship else {
        return;
    };
    if ships::base(&cx.logic.content.state, ShipKey::Npc(key)).is_some_and(|b| b.lane_entry != 0) {
        return;
    }
    if g.state == 12 {
        let mut list = DirectiveList::new();
        let mut d = Dock::default();
        d.op.dock_object = dock.dock_target;
        d.op.field_10 = 0;
        keep_and_queue(cx, g, DirectiveKind::Dock(d), &mut list);
        npc_ships::set_directives(cx, ShipKey::Npc(key), &list, 0);
        directives::clear_list(&mut cx.logic.content.state.directives, &mut list);
    }
    dock.dock_restart = false;
}
