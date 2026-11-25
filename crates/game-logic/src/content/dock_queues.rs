//! The dock queues of TU53 (`Content/DockQueues.cpp`, `DockQueues.h`; work
//! package P3.6a, decision record 0008 point 7).
//!
//! Five process-wide tables in [`ContentState`](super::ContentState)
//! ([`DockQueues`]):
//!
//! - `GDockWaitingShips`: ships waiting for a free terminal (response 3);
//! - `GDockingShips`: ships granted a terminal and flying in, ships
//!   launched from one, and ships that arrived through an airlock;
//! - `GDockTerminals`: the terminal states the pass marked, which
//!   SpaceSupport's handler tail hands to `pub::SpaceObj::Activate`
//!   ([`apply_terminal_states`]), and `GDockTerminalsApplied`, the states
//!   it handed over last;
//! - `GDockLaunchQueues`: the NPC ships' parameter blocks queued to launch
//!   from a dock target ([`QueuedLaunch`], P3.6c): [`queue_dock_launch`]
//!   and the launch pump [`launch_queued_ships`].
//!
//! A ship's request (`DockRequest`) asks [`handle_dock_request`]: the
//! ship's terminal ([`try_start_docking`], [`check_dock_permission`],
//! [`choose_dock_terminal`]), then `pub::SpaceObj::Dock` with the answer.
//! Each pass of [`update_dock_queues`] (SpaceSupport's heartbeat and the
//! dock messages) times a docking ship out after 90 s, marks the terminal
//! of a ship at its dock point occupied and frees the rest;
//! [`finish_docking`] drops an entry and grants the waiting ships their
//! terminals.
//!
//! **The lists.** Each list entry gets a serial key when it is made. The
//! C++ walks its `std::list`s with iterators and calls out while it holds
//! one; the port holds the key and looks the entry up again after every
//! call ([`DockList`]). A walk whose current entry a nested call erased,
//! which leaves the C++ with a dangling iterator, ends there (D842).
//!
//! **Content's ships.** The terminal requests read the ship's Content
//! object (`FSystemInfo::FindShip`): until P3.6c that is a player's ship
//! ([`super::ships`]), so an NPC ship the server made by itself is refused
//! with response 5, as the C++ refuses a ship Content does not know.

use std::collections::BTreeMap;

use neuron_core::math::{Matrix, Vector};

use super::ContentState;
use super::formations;
use super::npc_ships::NpcShipParams;
use super::players;
use super::ships::{self, ShipKey};
use crate::api;
use crate::api::ai::BaseOp;
use crate::api::space_obj::EqInfo;
use crate::events::ControllerMessage;
use crate::server::Cx;
use game_sim::ai::behavior::{DirectiveOp, DockOp};

/// `DOCK_HOST_RESPONSE` 0: the target is hostile.
pub const RESPONSE_HOSTILE: i32 = 0;
/// 1: access denied.
pub const RESPONSE_DENIED: i32 = 1;
/// 2: no terminal fits the ship.
pub const RESPONSE_NO_FIT: i32 = 2;
/// 3: every fitting terminal is busy, or the target moves.
pub const RESPONSE_BUSY: i32 = 3;
/// 4: proceed.
pub const RESPONSE_PROCEED: i32 = 4;
/// 5: the target has no terminal, or the ship is not Content's.
pub const RESPONSE_NONE: i32 = 5;

/// The time a docking ship has before it is kicked (`:595`, 90.0).
const DOCK_TIMEOUT: f64 = 90.0;

/// `GDockHostileAttitude` (`:42`): the attitude at or below which a dock
/// target refuses a ship.
pub const HOSTILE_ATTITUDE: f32 = -0.6;

/// The value of both dock lists (`FDockQueueEntry`, 0x28 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DockEntry {
    /// The port's serial key of the entry (no C++ field).
    pub key: u64,
    /// `SystemId` (`+0x00`).
    pub system: u32,
    /// `ShipId` (`+0x04`).
    pub ship: u32,
    /// `DockTargetId` (`+0x08`).
    pub target: u32,
    /// `DockIndex` (`+0x0c`), -1 for any terminal.
    pub index: i32,
    /// `+0x10`: in the waiting list a docking ring's terminal choice, in
    /// the docking list set once the ship is inside (it is then finished
    /// instead of timing out).
    pub inside: bool,
    /// `Time` (`+0x18`): the game time the entry was made.
    pub time: f64,
    /// `+0x20`: the entry never times out (the dock request's flag).
    pub no_timeout: bool,
}

/// `FDockTerminal` (0xc bytes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DockTerminal {
    /// `DockTargetId`.
    pub target: u32,
    /// `DockIndex`.
    pub index: i32,
    /// `bOccupied`.
    pub occupied: bool,
}

/// An NPC ship's parameters queued to launch from a dock target
/// (`FContentNpcShipParams`), holding its references.
pub type QueuedLaunch = NpcShipParams;

/// One of the two dock lists, its entries in list order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DockList {
    /// The entries.
    pub entries: Vec<DockEntry>,
}

impl DockList {
    fn position(&self, key: u64) -> Option<usize> {
        self.entries.iter().position(|e| e.key == key)
    }

    /// The entry of `key`.
    #[must_use]
    pub fn get(&self, key: u64) -> Option<&DockEntry> {
        self.entries.iter().find(|e| e.key == key)
    }

    /// The entry of `key`, to change.
    pub fn get_mut(&mut self, key: u64) -> Option<&mut DockEntry> {
        self.entries.iter_mut().find(|e| e.key == key)
    }

    /// `begin()`.
    #[must_use]
    pub fn first(&self) -> Option<u64> {
        self.entries.first().map(|e| e.key)
    }

    /// `++It`: the entry after `key`; `None` at the end, or when `key` is
    /// gone (D842).
    #[must_use]
    pub fn next(&self, key: u64) -> Option<u64> {
        let at = self.position(key)?;
        self.entries.get(at + 1).map(|e| e.key)
    }

    /// `erase(It)`: the entry after the erased one.
    pub fn erase(&mut self, key: u64) -> Option<u64> {
        let at = self.position(key)?;
        self.entries.remove(at);
        self.entries.get(at).map(|e| e.key)
    }

    /// `FindDockQueueEntry` (`:167-177`): the first entry of `ship`.
    #[must_use]
    pub fn find_ship(&self, ship: u32) -> Option<u64> {
        self.entries.iter().find(|e| e.ship == ship).map(|e| e.key)
    }

    /// The first entry of `ship` from `key` on.
    fn find_ship_from(&self, key: Option<u64>, ship: u32) -> Option<u64> {
        let at = self.position(key?)?;
        self.entries[at..]
            .iter()
            .find(|e| e.ship == ship)
            .map(|e| e.key)
    }
}

/// The dock queues' globals.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DockQueues {
    /// `GDockWaitingShips`.
    pub waiting: DockList,
    /// `GDockingShips`.
    pub docking: DockList,
    /// `GDockTerminals`: the states marked since the last hand-over.
    pub terminals: Vec<DockTerminal>,
    /// `GDockTerminalsApplied`: the states last handed to `Activate`.
    pub applied: Vec<DockTerminal>,
    /// `GDockLaunchQueues`: the launch queues by dock target.
    pub launch: BTreeMap<u32, Vec<QueuedLaunch>>,
    /// The next entry's serial key.
    next_key: u64,
}

impl DockQueues {
    /// `FDockQueueEntry()` (`:155-164`): ids 0, index -1, both flags clear,
    /// the time `time`; with the port's next key.
    pub fn new_entry(&mut self, time: f64) -> DockEntry {
        self.next_key += 1;
        DockEntry {
            key: self.next_key,
            system: 0,
            ship: 0,
            target: 0,
            index: -1,
            inside: false,
            time,
            no_timeout: false,
        }
    }

    /// `MarkDockTerminal` (`:135-152`): the first state of the pair only
    /// changes while its flag is clear; an unlisted pair is appended.
    pub fn mark_terminal(&mut self, target: u32, index: i32, occupied: bool) {
        if let Some(t) = self
            .terminals
            .iter_mut()
            .find(|t| t.target == target && t.index == index)
        {
            if !t.occupied {
                t.occupied = occupied;
            }
            return;
        }
        self.terminals.push(DockTerminal {
            target,
            index,
            occupied,
        });
    }

    /// `IsDockTerminalInUse` (`:82-92`): a docking entry holds the pair.
    #[must_use]
    pub fn terminal_in_use(&self, target: u32, index: i32) -> bool {
        self.docking
            .entries
            .iter()
            .any(|e| e.index == index && e.target == target)
    }

    /// The inline erase of the first state of `target` (`:530-541`).
    pub fn erase_first_terminal_of(terminals: &mut Vec<DockTerminal>, target: u32) {
        if let Some(at) = terminals.iter().position(|t| t.target == target) {
            terminals.remove(at);
        }
    }
}

fn queues<'c>(cx: &'c mut Cx<'_>) -> &'c mut DockQueues {
    &mut cx.logic.content.state.dock_queues
}

fn now(cx: &Cx<'_>) -> f64 {
    super::game_support::game_time(cx)
}

/// `pub::SpaceObj::GetTerminalInfo`'s type, as the C++ reads it whether
/// the call worked or not: a failed call leaves the C++'s local as it was
/// (uninitialised or an earlier value), which the port reads as 0 (D841).
fn terminal_type(cx: &Cx<'_>, target: u32, index: i32) -> u32 {
    api::space_obj::terminal_info(cx, target, index).map_or(0, |i| i.value)
}

/// `pub::SpaceObj::GetType`, 0 when it fails (it zeroes its output first).
fn object_type(cx: &Cx<'_>, object: u32) -> u32 {
    api::space_obj::object_type(cx, object).unwrap_or(0)
}

/// The ship class ids of the four terminal sizes (`GIdCanUse*`, TU54's
/// static ids).
fn can_use(name: &[u8]) -> u32 {
    neuron_core::id::create_id(name)
}

/// `DoesTerminalFitClasses` (`:59-79`): types 1, 2 and 6 need
/// `can_use_berths`, 3, 4 and 5 the small, medium and large moors; 7 and 8
/// take any ship, other types none.
#[must_use]
pub fn terminal_fits(terminal: u32, classes: &[u32]) -> bool {
    match terminal {
        1 | 2 | 6 => classes.contains(&can_use(b"can_use_berths")),
        3 => classes.contains(&can_use(b"can_use_small_moors")),
        4 => classes.contains(&can_use(b"can_use_medium_moors")),
        5 => classes.contains(&can_use(b"can_use_large_moors")),
        7 | 8 => true,
        _ => false,
    }
}

/// `GetShipDockingTarget` (`:435-456`): the first docking entry of the
/// ship: its target, its index and its terminal's type; `None` when the
/// ship is not docking.
#[must_use]
pub fn ship_docking_target(cx: &Cx<'_>, ship: u32) -> Option<(u32, i32, u32)> {
    let docking = &cx.logic.content.state.dock_queues.docking;
    let entry = docking.get(docking.find_ship(ship)?)?;
    let (target, index) = (entry.target, entry.index);
    Some((target, index, terminal_type(cx, target, index)))
}

/// `SetDockEquipment` (`:458-487`): the docking lights (`ActivateEquipment`
/// type 3) of the ship, or with `formation` of every member of its
/// formation in `system` (the ship alone when the formation has none; no
/// ship there, nothing). A missing record, which the C++ dereferences,
/// finds no ship (D843).
pub fn set_dock_equipment(
    cx: &mut Cx<'_>,
    ship: u32,
    on: bool,
    formation: bool,
    system: Option<u32>,
) {
    let info = EqInfo {
        kind: 3,
        on,
        count: 0,
        target: 0,
    };
    if !formation {
        let _ = api::space_obj::activate_equipment(cx, ship, &info);
        return;
    }
    let Some(key) = system.and_then(|s| ships::find_in_system(cx, s, ship)) else {
        return;
    };
    let state = &cx.logic.content.state;
    let mut members: Vec<u32> = formations::collect_members(state, ships::formation_id(state, key))
        .iter()
        .map(|&m| ships::id(state, m))
        .collect();
    if members.is_empty() {
        members.push(ships::id(state, key));
    }
    for member in members {
        let _ = api::space_obj::activate_equipment(cx, member, &info);
    }
}

/// `CancelShipDocking(Ship, PassThrough, bFormation)` (`:489-537`): the
/// ship's waiting entries go (with their docking lights, formation-wide);
/// a docking ship is answered `Dock(ship, target, -1, 5)` once and every
/// docking entry of it is finished. With `formation` and a system record
/// found at one of those entries, the other members of the ship's
/// formation are cancelled too. The pass-through argument is only handed to
/// those calls.
pub fn cancel_ship_docking(cx: &mut Cx<'_>, ship: u32, _pass_through: u32, formation: bool) {
    let mut system: Option<u32> = None;
    let mut found_record = false;
    let mut cursor = {
        let w = &queues(cx).waiting;
        w.find_ship(ship)
    };
    while let Some(key) = cursor {
        let Some(entry) = queues(cx).waiting.get(key).copied() else {
            break;
        };
        if entry.ship != ship {
            cursor = queues(cx).waiting.next(key);
            continue;
        }
        if !found_record {
            found_record = cx.logic.content.state.systems.contains_key(&entry.system);
            system = found_record.then_some(entry.system);
        }
        set_dock_equipment(cx, entry.ship, false, true, system);
        cursor = queues(cx).waiting.erase(key);
    }

    let first = queues(cx).docking.find_ship(ship);
    if let Some(first) = first {
        let target = queues(cx).docking.get(first).map_or(0, |e| e.target);
        let _ = api::space_obj::dock(cx, ship, target, -1, RESPONSE_NONE);
        let mut cursor = queues(cx).docking.find_ship_from(Some(first), ship);
        while let Some(key) = cursor {
            let Some(entry) = queues(cx).docking.get(key).copied() else {
                break;
            };
            if !found_record {
                found_record = cx.logic.content.state.systems.contains_key(&entry.system);
                system = found_record.then_some(entry.system);
            }
            let next = finish_docking(cx, key);
            cursor = next.and_then(|n| queues(cx).docking.find_ship_from(Some(n), ship));
        }
    }

    let Some(system) = system else {
        return;
    };
    if !formation {
        return;
    }
    // FindShip is not null-tested: a ship the record does not know reads
    // no formation (D843).
    let state_formation = ships::find_in_system(cx, system, ship).map_or(u32::MAX, |k| {
        ships::formation_id(&cx.logic.content.state, k)
    });
    let state = &cx.logic.content.state;
    let members: Vec<u32> = formations::collect_members(state, state_formation)
        .iter()
        .map(|&m| ships::id(state, m))
        .collect();
    for member in members {
        if member != ship {
            cancel_ship_docking(cx, member, _pass_through, false);
        }
    }
}

/// `SetObjectAttitude(ObjA, ObjB, Lo, Hi)` (`DockQueues.cpp:553-569`):
/// -2 when either object's reputation is unknown (`pub::SpaceObj::GetRep`);
/// else `Lo`, or with `Lo != Hi` (unordered too) `rand() / 32768` of the
/// way from `Lo` to `Hi` (in `double`, stored as a `float`), set as A's
/// attitude towards B (`pub::Reputation::SetAttitude`) and returned.
pub fn set_object_attitude(cx: &mut Cx<'_>, a: u32, b: u32, lo: f32, hi: f32) -> f32 {
    let (Ok(rep_a), Ok(rep_b)) = (api::space_obj::rep(cx, a), api::space_obj::rep(cx, b)) else {
        return -2.0;
    };
    let mut result = lo;
    if lo != hi {
        let random = f64::from(cx.world.rng().rand()) * (1.0 / 32768.0);
        result = (random * (f64::from(hi) - f64::from(lo)) + f64::from(lo)) as f32;
    }
    let _ = api::reputation::set_attitude(cx, rep_a, rep_b, result);
    result
}

/// `UpdateDockQueues` (`:581-656`): per docking entry, one that may time
/// out (neither flag set) and has waited more than 90 s kicks its ship
/// (a ship Content does not know is cancelled by id; a leader or a ship
/// without formation cancels its directive and docks again; a wingman
/// nothing); else a ship at its dock point marks its terminal occupied;
/// else a ship inside is finished; else its terminal is marked free. Then
/// the kicks.
pub fn update_dock_queues(cx: &mut Cx<'_>) {
    struct Kick {
        ship: Option<ShipKey>,
        ship_id: u32,
        target: u32,
        redock: bool,
    }
    let mut kicks = Vec::new();
    let mut cursor = queues(cx).docking.first();
    while let Some(key) = cursor {
        let Some(entry) = queues(cx).docking.get(key).copied() else {
            break;
        };
        if !entry.no_timeout && !entry.inside {
            let distance = super::game_support::time_distance(now(cx), entry.time);
            if distance > DOCK_TIMEOUT {
                // FindSystemInfo is not null-tested (D843).
                match ships::find_in_system(cx, entry.system, entry.ship) {
                    None => kicks.push(Kick {
                        ship: None,
                        ship_id: entry.ship,
                        target: entry.target,
                        redock: false,
                    }),
                    Some(ship) if !ships::is_wingman(&cx.logic.content.state, ship) => {
                        kicks.push(Kick {
                            ship: Some(ship),
                            ship_id: 0,
                            target: entry.target,
                            redock: true,
                        });
                    }
                    Some(_) => {}
                }
                cursor = queues(cx).docking.next(key);
                continue;
            }
        }
        if is_ship_at_dock_point(cx, entry.system, entry.ship, entry.target, entry.index) {
            queues(cx).mark_terminal(entry.target, entry.index, true);
            cursor = queues(cx).docking.next(key);
            continue;
        }
        if entry.inside {
            cursor = finish_docking(cx, key);
            continue;
        }
        queues(cx).mark_terminal(entry.target, entry.index, false);
        cursor = queues(cx).docking.next(key);
    }

    for kick in kicks {
        let Some(ship) = kick.ship else {
            cancel_ship_docking(cx, kick.ship_id, kick.target, false);
            continue;
        };
        let id = ships::id(&cx.logic.content.state, ship);
        let _ = api::ai::submit_directive(cx, id, &BaseOp::directive(DirectiveOp::Cancel));
        if kick.redock {
            let op = DockOp {
                dock_object: kick.target,
                ..DockOp::default()
            };
            let _ = api::ai::submit_directive(cx, id, &BaseOp::directive(DirectiveOp::Dock(op)));
        }
    }
}

/// `FinishDocking(&It)` (`:666-745`): the docking entry `key` goes (its
/// terminal marked free when nothing waits to launch there; the launch
/// queue's `operator[]` makes an empty one), then every waiting entry asks
/// for its terminal again. A grant is answered `Dock(.., 4)` and takes the
/// formation along (a gate, a hole or an airlock its followers, any other
/// target breaks the formation); 3 waits on; 0, 1, 2 and 5 are answered
/// and dropped. Returns the docking entry after the erased one.
pub fn finish_docking(cx: &mut Cx<'_>, key: u64) -> Option<u64> {
    let entry = queues(cx).docking.get(key).copied()?;
    let launches = queues(cx).launch.entry(entry.target).or_default();
    if launches.is_empty() {
        queues(cx).mark_terminal(entry.target, entry.index, false);
    }
    let next = queues(cx).docking.erase(key);

    let mut cursor = queues(cx).waiting.first();
    while let Some(wait_key) = cursor {
        let Some(wait) = queues(cx).waiting.get(wait_key).copied() else {
            break;
        };
        let mut index = -1;
        let mut response = RESPONSE_NONE;
        if let Ok(target_type) = api::space_obj::object_type(cx, wait.target) {
            if target_type & 0x20 != 0 {
                index = i32::from(wait.inside);
            }
            response = try_start_docking(
                cx,
                wait.system,
                wait.ship,
                wait.target,
                &mut index,
                wait.no_timeout,
            );
            if response == RESPONSE_PROCEED {
                // The comm system's "now cleared" event (type 2, waited in
                // the queue) is P3.6b's; its terminal type reads nothing.
                super::space_support::gcs_dock_granted_event(cx, &wait, index, response, true);
                let dock_type = object_type(cx, wait.target);
                let _ = api::space_obj::dock(cx, wait.ship, wait.target, index, RESPONSE_PROCEED);
                if dock_type & 0xc40 == 0 {
                    formations::break_formation_at_dock(cx, wait.system, wait.ship, false, true);
                } else {
                    formations::dock_formation_followers(
                        cx,
                        wait.system,
                        wait.ship,
                        wait.target,
                        index,
                    );
                }
                cursor = queues(cx).waiting.erase(wait_key);
                continue;
            }
        }
        match response {
            0 | 1 | 2 | 5 => {
                let _ = api::space_obj::dock(cx, wait.ship, wait.target, -1, response);
                cursor = queues(cx).waiting.erase(wait_key);
            }
            3 => cursor = queues(cx).waiting.next(wait_key),
            // TryStartDocking answers 0 to 5 only.
            _ => cursor = queues(cx).waiting.next(wait_key),
        }
    }
    // The erased entry's successor, looked up again: a nested call may
    // have erased it, where the C++ keeps a dangling iterator (D842).
    next.filter(|&n| queues(cx).docking.get(n).is_some())
}

/// `TryStartDocking(System, &Ship, &Target, &Index, bFlag)` (`:747-778`):
/// 5 for a ship Content does not know in the system; otherwise the
/// permission ([`check_dock_permission`]), and on 4 the ship enters the
/// docking list with the flag. A missing record, which the C++
/// dereferences, knows no ship (D843).
pub fn try_start_docking(
    cx: &mut Cx<'_>,
    system: u32,
    ship: u32,
    target: u32,
    index: &mut i32,
    no_timeout: bool,
) -> i32 {
    let Some(key) = ships::find_in_system(cx, system, ship) else {
        return RESPONSE_NONE;
    };
    let position = ships::position(cx, key);
    let state = &cx.logic.content.state;
    let rep = ships::reputation(state, key);
    let classes = ships::base(state, key)
        .map(|s| s.class_ids.clone())
        .unwrap_or_default();
    let response = check_dock_permission(cx, &classes, rep, ship, Some(position), target, index);
    if response != RESPONSE_PROCEED {
        return response;
    }
    let time = now(cx);
    let ship_id = ships::id(&cx.logic.content.state, key);
    let q = queues(cx);
    let mut entry = q.new_entry(time);
    entry.system = system;
    entry.ship = ship_id;
    entry.target = target;
    entry.index = *index;
    entry.inside = false;
    entry.no_timeout = no_timeout;
    q.docking.entries.push(entry);
    response
}

/// `IsGateLockedForShip(Ship, Gate)` (`Content/SpaceSupport.cpp:1320-1342`):
/// false for ship 0; a player's ship asks the player's gate locks; any
/// other needs the gate's system record and is locked by the NPC locks,
/// unless the record knows it as a ship that is not SpacePop's (P3.6c:
/// the record knows no NPC ship yet).
#[must_use]
pub fn is_gate_locked_for_ship(cx: &Cx<'_>, ship: u32, gate: u32) -> bool {
    if ship == 0 {
        return false;
    }
    let state = &cx.logic.content.state;
    if let Some(player) = players::find_by_ship(&state.players, ship) {
        return state.gate_locks.is_locked(player, gate);
    }
    let system = api::space_obj::system(cx, gate).unwrap_or(0);
    if system & 0x3fff_ffff == 0 || !state.systems.contains_key(&system) {
        return false;
    }
    // A ship the record finds here is a player's, and players were asked
    // above; the NPC ships' `IsSpacePopShip` test comes with P3.6c.
    state.gate_locks.is_npc_locked(gate)
}

/// `CheckDockPermission(&Classes, &ShipRep, &Ship, Position, &Target,
/// &Index)` (`:787-855`): a locked gate or a player's refused dock is 1
/// (the player hears `dock_disallowed` on a live mission when refused,
/// else `info_access_denied`); a target other than a gate, an airlock or a
/// hole whose attitude towards the ship is at or below -0.6 is 0 (a player
/// hears `nnv_dock_denied_destination_hostile`); then the players locked
/// out of a gate leave the ship's formation, a terminal is chosen, and a
/// granted terminal of a target that moves faster than 0.01 is 3.
pub fn check_dock_permission(
    cx: &mut Cx<'_>,
    classes: &[u32],
    ship_rep: i32,
    ship: u32,
    position: Option<Vector>,
    target: u32,
    index: &mut i32,
) -> i32 {
    let state = &cx.logic.content.state;
    let player = players::find_by_ship(&state.players, ship);
    let may_dock = player
        .and_then(|p| state.players.get(&p))
        .is_none_or(|p| p.can_dock_at(target));
    if is_gate_locked_for_ship(cx, ship, target) || (player.is_some() && !may_dock) {
        if let Some(player) = player {
            let live = cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .map_or(0, |p| p.live_mission);
            let message = if live != 0 && !may_dock {
                api::misc::nickname_id(b"dock_disallowed")
            } else {
                api::misc::nickname_id(b"info_access_denied")
            };
            let _ = api::player::send_nn_message(cx, player, message);
        }
        *index = -1;
        return RESPONSE_DENIED;
    }
    let target_type = object_type(cx, target);
    if target_type & 0xc40 == 0 {
        let mut attitude = 1.0_f32;
        if ship_rep != -1 {
            // The target's rep starts at 0, which a failed GetRep leaves.
            let target_rep = api::space_obj::rep(cx, target).unwrap_or(0);
            if let Ok(a) = api::reputation::attitude(cx, target_rep, ship_rep) {
                attitude = a;
            }
        }
        if attitude.partial_cmp(&HOSTILE_ATTITUDE) != Some(std::cmp::Ordering::Greater) {
            *index = -1;
            if let Some(player) = player {
                let message = api::misc::nickname_id(b"nnv_dock_denied_destination_hostile");
                let _ = api::player::send_nn_message(cx, player, message);
            }
            return RESPONSE_HOSTILE;
        }
    }
    formations::drop_players_locked_out_of_gate(cx, ship, target);
    let response = choose_dock_terminal(cx, classes, target, position, index);
    if response != RESPONSE_PROCEED {
        return response;
    }
    let Ok((linear, angular)) = api::space_obj::motion(cx, target) else {
        return RESPONSE_PROCEED;
    };
    let speed = |v: Vector| {
        (f64::from(v.y) * f64::from(v.y)
            + f64::from(v.z) * f64::from(v.z)
            + f64::from(v.x) * f64::from(v.x))
        .sqrt()
    };
    if speed(linear) > 0.01 || speed(angular) > 0.01 {
        return RESPONSE_BUSY;
    }
    RESPONSE_PROCEED
}

/// `ChooseDockTerminal(&Classes, &Target, Position, &Index)` (`:865-967`):
/// with index -1 every terminal that fits the classes and is free (none
/// at all: 5; none fitting: 2), else the given terminal (unknown: 5, not
/// fitting: 2, free: a candidate). No candidate: 3. Then 4 with the given
/// index, the candidate whose hardpoint is nearest `position` (index -1
/// with a position), or a random candidate (`rand() * n / 32768`, no draw
/// for one). Every refusal stores -1.
pub fn choose_dock_terminal(
    cx: &mut Cx<'_>,
    classes: &[u32],
    target: u32,
    position: Option<Vector>,
    index: &mut i32,
) -> i32 {
    let mut candidates: Vec<i32> = Vec::new();
    if *index == -1 {
        let Ok(mut info) = api::space_obj::terminal_info(cx, target, 0) else {
            *index = -1;
            return RESPONSE_NONE;
        };
        let mut at = 0;
        let mut any_fits = false;
        loop {
            if terminal_fits(info.value, classes) {
                any_fits = true;
                if !cx
                    .logic
                    .content
                    .state
                    .dock_queues
                    .terminal_in_use(target, at)
                {
                    candidates.push(at);
                }
            }
            at += 1;
            match api::space_obj::terminal_info(cx, target, at) {
                Ok(next) => info = next,
                Err(_) => break,
            }
        }
        if !any_fits {
            *index = -1;
            return RESPONSE_NO_FIT;
        }
    } else {
        let Ok(info) = api::space_obj::terminal_info(cx, target, *index) else {
            *index = -1;
            return RESPONSE_NONE;
        };
        if !terminal_fits(info.value, classes) {
            *index = -1;
            return RESPONSE_NO_FIT;
        }
        if !cx
            .logic
            .content
            .state
            .dock_queues
            .terminal_in_use(target, *index)
        {
            candidates.push(*index);
        }
    }
    if candidates.is_empty() {
        *index = -1;
        return RESPONSE_BUSY;
    }
    if *index == -1
        && let Some(position) = position
    {
        let mut best = -1;
        let mut best_distance = f32::MAX;
        // One local across the loop: a failed GetHardpoint leaves the last
        // difference in it, as the C++'s (the first value is the port's
        // zero, where the C++'s slot is unwritten).
        let mut work = Vector::ZERO;
        for &candidate in &candidates {
            let name = api::space_obj::terminal_info(cx, target, candidate)
                .map(|i| i.name)
                .unwrap_or_default();
            if let Ok((p, _)) = api::space_obj::hardpoint(cx, target, &name) {
                work = p;
            }
            work.x -= position.x;
            work.y -= position.y;
            work.z -= position.z;
            let distance = f64::from(work.x) * f64::from(work.x)
                + f64::from(work.z) * f64::from(work.z)
                + f64::from(work.y) * f64::from(work.y);
            if distance < f64::from(best_distance) {
                best = candidate;
                best_distance = distance as f32;
            }
        }
        *index = best;
        return RESPONSE_PROCEED;
    }
    let count = candidates.len() as i32;
    let mut pick = 0;
    if count - 1 != 0 {
        pick = cx.world.rng().rand() * count / 32768;
    }
    *index = candidates[pick as usize];
    RESPONSE_PROCEED
}

/// The inverse-square-root idiom (`ApproximateSqrt`, `:116-125`).
fn approximate_sqrt(stored_square: f32, square: f64) -> f64 {
    let bits = 0x5f37_59df_i32.wrapping_sub(stored_square.to_bits() as i32 >> 1);
    let y = f64::from(f32::from_bits(bits as u32));
    let step = 1.5 - y * y * square * 0.5;
    step * y * square
}

/// `DistanceSquared` (`:105-111`): each difference stored as a float, the
/// squares summed in extended precision (`f64`).
fn distance_squared(a: Vector, b: Vector) -> f64 {
    let x = a.x - b.x;
    let y = a.y - b.y;
    let z = a.z - b.z;
    f64::from(z) * f64::from(z) + f64::from(y) * f64::from(y) + f64::from(x) * f64::from(x)
}

/// `IsShipAtDockPoint(System, &Ship, &Target, &Index)` (`:976-1059`): the
/// terminal's hardpoint and the ship's location (the ship known to Content
/// in the system), any failure false; terminals 1, 3, 4, 5 within 200 of
/// the hardpoint, 7 within 2500, 8 within 500; a berth (6) at the
/// hardpoint in x and z, on the segment 5000 behind it, or within 1.85
/// times the target's radius; any other type false.
pub fn is_ship_at_dock_point(cx: &Cx<'_>, system: u32, ship: u32, target: u32, index: i32) -> bool {
    let Ok(info) = api::space_obj::terminal_info(cx, target, index) else {
        return false;
    };
    let Ok((hardpoint, orientation)) = api::space_obj::hardpoint(cx, target, &info.name) else {
        return false;
    };
    // FindSystemInfo is not null-tested (D843).
    if ships::find_in_system(cx, system, ship).is_none() {
        return false;
    }
    let Ok((at, _)) = api::space_obj::location(cx, ship) else {
        return false;
    };
    match info.value {
        1 | 3 | 4 | 5 => {
            let d = Vector::new(at.x - hardpoint.x, at.y - hardpoint.y, at.z - hardpoint.z);
            return (f64::from(d.z) * f64::from(d.z) + f64::from(d.y) * f64::from(d.y))
                + f64::from(d.x) * f64::from(d.x)
                < 40000.0;
        }
        7 => return distance_squared(at, hardpoint) < 6_250_000.0,
        8 => return distance_squared(at, hardpoint) < 250_000.0,
        6 => {}
        _ => return false,
    }
    if (f64::from(hardpoint.x) - f64::from(at.x)).abs() < 10.0
        && (f64::from(hardpoint.z) - f64::from(at.z)).abs() < 10.0
    {
        return true;
    }
    let far = transform_point(&orientation, hardpoint, Vector::new(0.0, 0.0, -5000.0));
    let to_far_square = distance_squared(at, far);
    let to_far_stored = to_far_square as f32;
    let to_hardpoint_square = distance_squared(at, hardpoint);
    let to_hardpoint = approximate_sqrt(to_hardpoint_square as f32, to_hardpoint_square);
    let to_far = approximate_sqrt(to_far_stored, f64::from(to_far_stored));
    if (5000.0 - (to_far + to_hardpoint)).abs() < 25.0 {
        return true;
    }
    let radius = api::space_obj::radius(cx, target).map_or(0.0, |(r, _)| r);
    let dx = f64::from(at.x) - f64::from(hardpoint.x);
    let dy = f64::from(at.y) - f64::from(hardpoint.y);
    let dz = f64::from(at.z) - f64::from(hardpoint.z);
    let square = dz * dz + dy * dy + dx * dx;
    let distance = approximate_sqrt(square as f32, square);
    f64::from(radius) * f64::from(1.85_f32) > distance
}

/// `I3DMathEngine::TransformXform(Out, {Rotation, Translation}, V)`: the
/// rotation applied to `v` (row by row, each product summed and stored as
/// a float), plus the translation.
fn transform_point(rotation: &Matrix, translation: Vector, v: Vector) -> Vector {
    let row = |r: [f32; 3]| {
        (f64::from(r[0]) * f64::from(v.x)
            + f64::from(r[1]) * f64::from(v.y)
            + f64::from(r[2]) * f64::from(v.z)) as f32
    };
    let m = rotation.m;
    Vector::new(
        row(m[0]) + translation.x,
        row(m[1]) + translation.y,
        row(m[2]) + translation.z,
    )
}

/// `GetDockTypeForClasses(&Target, &Classes)` (`:1062-1081`): 7 for a gate
/// or a hole, 8 for an airlock, a docking ring 6 (berths) or 0, else 1, 3,
/// 4, 5 by the first class the ship has, or 0.
#[must_use]
pub fn dock_type_for_classes(cx: &Cx<'_>, target: u32, classes: &[u32]) -> i32 {
    let target_type = object_type(cx, target);
    let has = |name: &[u8]| classes.contains(&can_use(name));
    if target_type & 0x840 != 0 {
        return 7;
    }
    if target_type & 0x400 != 0 {
        return 8;
    }
    if target_type & 0x20 != 0 {
        return if has(b"can_use_berths") { 6 } else { 0 };
    }
    if has(b"can_use_berths") {
        1
    } else if has(b"can_use_small_moors") {
        3
    } else if has(b"can_use_medium_moors") {
        4
    } else if has(b"can_use_large_moors") {
        5
    } else {
        0
    }
}

/// `HandleDockRequest(&Request)` (`:1380-1460`), SpaceSupport's
/// `DockRequest`: a docking ring asks for terminal 0, anything else for
/// any; the answer of [`try_start_docking`] goes to the comm system (any
/// answer but 5; P3.6b) and to the ship: 4 docks it (a gate, a hole or an
/// airlock takes its followers along, anything else breaks its formation,
/// releasing the players at a station or a planet), 3 puts it in the
/// waiting list (behind the players already waiting when a player is in
/// it or its formation, else at the end) and answers 3 with index -1, the
/// rest is answered with the index. Returns the answer.
pub fn handle_dock_request(
    cx: &mut Cx<'_>,
    ship: u32,
    target: u32,
    system: u32,
    flag: bool,
) -> i32 {
    let target_type = object_type(cx, target);
    let mut index = if target_type & 0x20 != 0 { 0 } else { -1 };
    let response = try_start_docking(cx, system, ship, target, &mut index, flag);
    if response != RESPONSE_NONE {
        let time = now(cx);
        let mut event = queues(cx).new_entry(time);
        event.system = system;
        event.ship = ship;
        event.target = target;
        super::space_support::gcs_dock_granted_event(cx, &event, index, response, false);
    }
    if response == RESPONSE_PROCEED {
        let dock_type = object_type(cx, target);
        let _ = api::space_obj::dock(cx, ship, target, index, RESPONSE_PROCEED);
        if dock_type & 0xc40 != 0 {
            formations::dock_formation_followers(cx, system, ship, target, index);
        } else {
            formations::break_formation_at_dock(cx, system, ship, false, dock_type & 0x120 != 0);
        }
        return response;
    }
    if response == RESPONSE_BUSY {
        let time = now(cx);
        let state = &cx.logic.content.state;
        // A ship of the record's space objects whose creation kind is 0 or
        // 1 is placed behind the players too (P3.6c: none yet).
        let behind = players::find_by_ship(&state.players, ship).is_some()
            || formations::is_player_in_ship_formation(cx, system, ship);
        let q = queues(cx);
        let mut entry = q.new_entry(time);
        entry.system = system;
        entry.ship = ship;
        entry.target = target;
        entry.index = index;
        let mut at = q.waiting.entries.len();
        if behind {
            let players = &cx.logic.content.state.players;
            let waiting = &cx.logic.content.state.dock_queues.waiting;
            at = waiting
                .entries
                .iter()
                .position(|e| players::find_by_ship(players, e.ship).is_none())
                .unwrap_or(waiting.entries.len());
        }
        queues(cx).waiting.entries.insert(at, entry);
        let _ = api::space_obj::dock(cx, ship, target, -1, RESPONSE_BUSY);
        return response;
    }
    if matches!(response, 0 | 1 | 2 | 5) {
        let _ = api::space_obj::dock(cx, ship, target, index, response);
    }
    response
}

/// `ApplyDockTerminalStates` (`:1740-1789`), SpaceSupport's handler tail
/// for every message but `JumpgateAct`: each marked state that differs
/// from the one handed over last (or is new) goes to
/// `pub::SpaceObj::Activate(Target, Occupied, Index)`, and on success is
/// recorded; the marks are emptied, then controller 0 hears `JumpgateAct`
/// for each target that became occupied, in order.
pub fn apply_terminal_states(cx: &mut Cx<'_>) {
    let pending = std::mem::take(&mut queues(cx).terminals);
    let mut occupied = Vec::new();
    for state in &pending {
        let applied_at = queues(cx)
            .applied
            .iter()
            .position(|a| a.target == state.target && a.index == state.index);
        if let Some(at) = applied_at {
            if queues(cx).applied[at].occupied == state.occupied {
                continue;
            }
            if api::space_obj::activate(cx, state.target, state.occupied, state.index).is_err() {
                continue;
            }
            if let Some(a) = queues(cx).applied.get_mut(at) {
                a.occupied = state.occupied;
            }
        } else {
            if api::space_obj::activate(cx, state.target, state.occupied, state.index).is_err() {
                continue;
            }
            queues(cx).applied.push(*state);
        }
        if state.occupied {
            occupied.push(state.target);
        }
    }
    // The marks made by the calls above (none: Activate marks nothing)
    // are dropped with the C++'s `clear()` of the same vector.
    queues(cx).terminals.clear();
    for target in occupied {
        let mut message = ControllerMessage::JumpgateAct { target };
        let _ = cx.dispatch(
            neuron_server::controller::ControllerId::BROADCAST,
            &mut message,
        );
    }
}

/// `FDockQueueEntry` for a ship that enters the docking list as launched
/// (`inside` set): SpaceSupport's `SpaceEnter` and `SystemEnter`.
pub fn push_launched(cx: &mut Cx<'_>, system: u32, ship: u32, target: u32, index: i32) {
    let time = now(cx);
    let q = queues(cx);
    let mut entry = q.new_entry(time);
    entry.system = system;
    entry.target = target;
    entry.index = index;
    entry.ship = ship;
    entry.inside = true;
    q.docking.entries.push(entry);
}

/// The docking entry of the leader for [`formations`]: the first entry of
/// `ship`, copied.
#[must_use]
pub fn docking_entry_of(state: &ContentState, ship: u32) -> Option<DockEntry> {
    let docking = &state.dock_queues.docking;
    docking.get(docking.find_ship(ship)?).copied()
}

/// `AddLeadingDirective(Params, Directive)`: the directive at the front of
/// the block's list, one reference taken (the creator's is released after).
fn add_leading_directive(
    cx: &mut Cx<'_>,
    params: &mut NpcShipParams,
    kind: super::mission::obj_list::DirectiveKind,
) {
    let arena = &mut cx.logic.content.state.directives;
    let d = arena.create(kind);
    params.directives.insert(0, d);
    arena.add_ref(d);
    arena.release(d);
}

/// `PlaceLaunchedShip(Params, &TargetType, TerminalType, &Position,
/// &Orientation)` (`:1865-1950`): where a ship launched from a terminal
/// starts. From a docking ring (type `0x20`) the hardpoint lifted by
/// `rand() / 32768 * 10000 + 10000`; else the hardpoint's orientation
/// (turned a quarter about its up axis unless the terminal is a mooring of
/// type 3 to 5) with the archetype's `HpMount` at the hardpoint.
fn place_launched_ship(
    cx: &mut Cx<'_>,
    params: &mut NpcShipParams,
    target_type: u32,
    terminal_type: u32,
    hardpoint: Vector,
    hardpoint_orientation: &Matrix,
) {
    fn e(v: f32) -> f64 {
        f64::from(v)
    }
    let r = |v: f64| v as f32;
    if target_type & 0x20 != 0 {
        let random = cx.world.rng().rand();
        let offset_y = r(f64::from(random) * e(3.051_757_8e-5_f32) * 10000.0 + e(10000.0_f32));
        params.position = Vector::new(
            r(e(hardpoint.x) + 0.0),
            r(e(hardpoint.y) + e(offset_y)),
            r(e(hardpoint.z) + 0.0),
        );
        return;
    }
    params.orientation = *hardpoint_orientation;
    if !matches!(terminal_type, 3..=5) {
        let m = &params.orientation.m;
        let axis = Vector::new(m[0][1], m[1][1], m[2][1]);
        let sin_half = std::f64::consts::FRAC_PI_2.sin() as f32;
        let length = (e(axis.x) * e(axis.x) + e(axis.y) * e(axis.y) + e(axis.z) * e(axis.z)).sqrt();
        let scale = r(e(sin_half) / length);
        let quat = neuron_core::math::Quaternion {
            w: std::f64::consts::FRAC_PI_2.cos() as f32,
            x: r(e(scale) * e(axis.x)),
            y: r(e(scale) * e(axis.y)),
            z: r(e(scale) * e(axis.z)),
        };
        let rotation = neuron_core::math::quaternion_to_matrix(&quat);
        params.orientation = neuron_core::math::mul_matrix(&rotation, &params.orientation);
    }
    // `pub::FindHardpoint("HpMount", ShipArchId)`, the result not tested
    // (the outputs stay zero on a failure).
    let mount = api::misc::find_hardpoint(cx, b"HpMount", params.archetype)
        .map_or(Vector::ZERO, |t| t.translation);
    let current = params.orientation;
    let rotated = neuron_core::math::transform_matrix(&current, mount);
    let mount_point = Vector::new(
        r(e(rotated.x) + e(hardpoint.x)),
        r(e(rotated.y) + e(hardpoint.y)),
        r(e(rotated.z) + e(hardpoint.z)),
    );
    let offset = Vector::new(
        r(e(hardpoint.x) - e(mount_point.x)),
        r(e(hardpoint.y) - e(mount_point.y)),
        r(e(hardpoint.z) - e(mount_point.z)),
    );
    let transposed = current.transpose();
    let local = neuron_core::math::transform_matrix(&transposed, offset);
    let back = neuron_core::math::transform_matrix(&current, local);
    params.position = Vector::new(
        r(e(back.x) + e(hardpoint.x)),
        r(e(back.y) + e(hardpoint.y)),
        r(e(back.z) + e(hardpoint.z)),
    );
}

/// The whole queue of `target` erased (`EraseLaunchQueue`), each block's
/// references released.
fn erase_launch_queue(cx: &mut Cx<'_>, target: u32) {
    let blocks = queues(cx)
        .launch
        .get_mut(&target)
        .map(std::mem::take)
        .unwrap_or_default();
    for block in blocks {
        super::npc_ships::release_params(cx, block);
    }
}

/// Every launch queue erased (`EraseAllDockLaunchQueues`).
pub fn erase_all_launch_queues(cx: &mut Cx<'_>) {
    let targets: Vec<u32> = queues(cx).launch.keys().copied().collect();
    for target in targets {
        erase_launch_queue(cx, target);
    }
    queues(cx).launch.clear();
}

/// `GDockLaunchQueues.erase(Object)`: the object's queue, its blocks'
/// references released.
pub fn erase_launch_queue_of(cx: &mut Cx<'_>, target: u32) {
    erase_launch_queue(cx, target);
    queues(cx).launch.remove(&target);
}

/// `QueueDockLaunch(Params, DockTarget, OutShip)` (`:1959-1968`): the
/// block (with its references) at the end of the dock target's queue, the
/// queues launched at once; true while the target's queue still holds
/// blocks. The ship of the launch pass's result goes out.
pub fn queue_dock_launch(
    cx: &mut Cx<'_>,
    params: &NpcShipParams,
    dock_target: u32,
) -> (bool, Option<crate::events::NpcShipKey>) {
    let copy = params.clone();
    {
        let state = &mut cx.logic.content.state;
        if let Some(h) = copy.reputation {
            state.rep_handles.add_ref(h);
        }
        for &d in &copy.directives {
            state.directives.add_ref(d);
        }
    }
    queues(cx).launch.entry(dock_target).or_default().push(copy);
    let ship = launch_queued_ships(cx);
    let queued = queues(cx).launch.entry(dock_target).or_default().is_empty();
    (!queued, ship)
}

/// `LaunchQueuedShips` (`:1999-2140`), SpaceSupport's heartbeat and
/// [`queue_dock_launch`]: every queued block is checked against its dock
/// target and made: through a gate, a hole or an airlock (`0xc40`) a 2 s
/// delay leads the directives of a block that is no formation follower;
/// from a base or a docking ring (`0x600de0`) the dock permission decides
/// (0 and 2 drop the block, 5 the queue, 3 waits unless the ship's life is
/// set, 4 reads the terminal's hardpoint) before the ship is placed at the
/// terminal and given a launch directive. The ship is made, joins its
/// system's space objects, `NpcShipCreated` goes to every controller, and
/// it jumps in or launches (entering the docking list). Returns the last
/// ship made whose life is set (`+0x104 >= 0`).
///
/// The C++'s loop steps once more after an erase: the block after a made
/// one waits for the next pass, and the last one wraps to the queue's
/// first (kept). The terminal locals carry over between blocks, as the
/// C++'s function-scope locals do. The comm system's hail silence and
/// launch conversation are P3.6b's (D844).
#[allow(clippy::too_many_lines)]
pub fn launch_queued_ships(cx: &mut Cx<'_>) -> Option<crate::events::NpcShipKey> {
    let mut result = None;
    let mut info = api::space_obj::TerminalInfo {
        name: Vec::new(),
        value: 0,
    };
    let mut hardpoint = Vector::ZERO;
    let mut hardpoint_orientation = Matrix::from_rows([[0.0; 3]; 3]);
    let targets: Vec<u32> = queues(cx).launch.keys().copied().collect();
    for target in targets {
        let mut at = 0_usize;
        loop {
            let len = queues(cx).launch.get(&target).map_or(0, Vec::len);
            if at >= len {
                break;
            }
            let target_type = match api::space_obj::object_type(cx, target) {
                Ok(t) => t,
                Err(_) => {
                    erase_launch_queue(cx, target);
                    break;
                }
            };
            let Some(mut params) = queues(cx).launch.get_mut(&target).map(|q| q.remove(at)) else {
                break;
            };
            let mut index = params.field_0x110;
            if target_type & 0xc40 != 0 {
                if params.formation_slot == 0 || params.formation_id == u32::MAX {
                    let delay = super::mission::obj_list::Delay {
                        delay_seconds: 2.0,
                        ..super::mission::obj_list::Delay::default()
                    };
                    add_leading_directive(
                        cx,
                        &mut params,
                        super::mission::obj_list::DirectiveKind::Delay(delay),
                    );
                }
            } else if target_type & 0x60_0de0 != 0 {
                if target_type & 0x20 != 0 {
                    index = 1;
                }
                let ship_rep = cx.logic.content.state.rep_handles.vibe(params.reputation);
                let response = check_dock_permission(
                    cx,
                    &params.class_ids.clone(),
                    ship_rep,
                    0,
                    None,
                    target,
                    &mut index,
                );
                match response {
                    0 | 2 => {
                        super::npc_ships::release_params(cx, params);
                        continue;
                    }
                    5 => {
                        super::npc_ships::release_params(cx, params);
                        erase_launch_queue(cx, target);
                        break;
                    }
                    3 => {
                        if params.life_time >= 0.0 {
                            super::npc_ships::release_params(cx, params);
                            erase_launch_queue(cx, target);
                            break;
                        }
                        if let Some(q) = queues(cx).launch.get_mut(&target) {
                            q.insert(at, params);
                        }
                        at += 1;
                        continue;
                    }
                    4 => {
                        if let Ok(i) = api::space_obj::terminal_info(cx, target, index) {
                            info = i;
                        }
                        if let Ok((p, o)) = api::space_obj::hardpoint(cx, target, &info.name) {
                            hardpoint = p;
                            hardpoint_orientation = o;
                        }
                    }
                    _ => {}
                }
                place_launched_ship(
                    cx,
                    &mut params,
                    target_type,
                    info.value,
                    hardpoint,
                    &hardpoint_orientation,
                );
                let open_terminal = !matches!(info.value, 2..=5);
                if params.life_time.to_bits() != (-1.0_f32).to_bits() || open_terminal {
                    let mut launch = super::mission::obj_list::Launch::default();
                    launch.op.launch_object = target;
                    launch.op.index = index;
                    launch.op.terminal = info.value as i32;
                    add_leading_directive(
                        cx,
                        &mut params,
                        super::mission::obj_list::DirectiveKind::Launch(launch),
                    );
                }
            }
            params.field_0x110 = index;
            params.guide_target = target;
            let system = params.system.unwrap_or(0);
            let ship = super::npc_ships::construct(cx, &params);
            if params.life_time >= 0.0 {
                result = Some(ship);
            }
            if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
                record.space_objs.push(ship);
            }
            if let Some(s) = cx.logic.content.state.npc_ships.get_mut(&ship) {
                s.base.system = params.system;
            }
            let _ = cx.dispatch(
                neuron_server::controller::ControllerId::BROADCAST,
                &mut ControllerMessage::NpcShipCreated { ship },
            );
            let ship_id = super::npc_ships::space_obj(cx, ship);
            if target_type & 0xc40 != 0 {
                let _ = api::space_obj::jump_in(cx, ship_id, target);
            } else if target_type & 0x60_0de0 != 0 {
                let time = now(cx);
                let q = queues(cx);
                let mut entry = q.new_entry(time);
                entry.system = system;
                entry.index = index;
                entry.target = target;
                entry.ship = ship_id;
                entry.inside = true;
                q.docking.entries.push(entry);
                // The dock hail's silence and the launch conversation (the
                // terminal info read again): P3.6b (D844).
                if let Ok(i) = api::space_obj::terminal_info(cx, target, index) {
                    info = i;
                }
                if target_type & 0x100 == 0 {
                    let _ = api::space_obj::launch(cx, ship_id, target, 0);
                }
            }
            super::npc_ships::release_params(cx, params);
            // The C++'s increment after the erase: skip one, or wrap to the
            // first after the last.
            let len = queues(cx).launch.get(&target).map_or(0, Vec::len);
            if at >= len {
                at = 0;
            } else {
                at += 1;
            }
        }
        if queues(cx).launch.get(&target).is_some_and(Vec::is_empty) {
            queues(cx).launch.remove(&target);
        }
    }
    result
}

/// `PlaceFormationBlocks(&Blocks, Formation, &Placement)` (`:2169-2212`):
/// the blocks in a new formation (every block whose slot is no player slot
/// takes its slot, the formation and the new id), then each placed at its
/// slot's transform from the placement. A block whose slot the formation
/// rejects keeps the previous block's transform (zero for the first).
pub fn place_formation_blocks(
    cx: &mut Cx<'_>,
    blocks: &mut [NpcShipParams],
    formation: Option<u32>,
    placement: &neuron_core::math::Transform,
) {
    let id = formations::allocate_id(&mut cx.logic.content.state);
    let tables = cx.logic.content.tables.clone();
    let layout = formation.and_then(|f| tables.formation(f));
    for (slot, block) in blocks.iter_mut().enumerate() {
        // `IsPlayerSlot` without a bound (D840: past the end, none).
        let player_slot = layout
            .and_then(|f| f.slots.get(slot))
            .is_some_and(|s| s.player_slot);
        if !player_slot {
            block.formation_slot = slot as u32;
            block.formation = formation;
            block.formation_id = id;
        }
    }
    let mut position = Vector::ZERO;
    let mut orientation = Matrix::from_rows([[0.0; 3]; 3]);
    for block in blocks.iter_mut() {
        if let Some(s) = layout.and_then(|f| f.slots.get(block.formation_slot as usize)) {
            // The formation's own transform is the identity and zero: the
            // slot's position (`-0` reads `+0`) and the identity.
            position = Vector::new(
                s.position[0] + 0.0,
                s.position[1] + 0.0,
                s.position[2] + 0.0,
            );
            orientation = Matrix::IDENTITY;
        }
        block.position = neuron_core::math::transform_xform(placement, position);
        block.orientation = neuron_core::math::mul_matrix(&placement.rotation, &orientation);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_marked_terminal_stays_occupied_until_it_is_handed_over() {
        let mut q = DockQueues::default();
        q.mark_terminal(7, 0, true);
        q.mark_terminal(7, 0, false);
        q.mark_terminal(7, 1, false);
        assert_eq!(
            q.terminals,
            [
                DockTerminal {
                    target: 7,
                    index: 0,
                    occupied: true
                },
                DockTerminal {
                    target: 7,
                    index: 1,
                    occupied: false
                }
            ]
        );
    }

    #[test]
    fn the_lists_walk_by_key() {
        let mut q = DockQueues::default();
        let (a, b, c) = (q.new_entry(0.0), q.new_entry(0.0), q.new_entry(0.0));
        q.waiting.entries.extend([a, b, c]);
        assert_eq!(q.waiting.first(), Some(a.key));
        assert_eq!(q.waiting.erase(b.key), Some(c.key));
        assert_eq!(q.waiting.next(a.key), Some(c.key));
        assert_eq!(q.waiting.next(b.key), None);
        assert_eq!(q.waiting.erase(c.key), None);
    }

    #[test]
    fn terminals_fit_by_class() {
        let berths = [can_use(b"can_use_berths")];
        assert!(terminal_fits(1, &berths));
        assert!(terminal_fits(6, &berths));
        assert!(!terminal_fits(3, &berths));
        assert!(terminal_fits(7, &[]));
        assert!(terminal_fits(8, &[]));
        assert!(!terminal_fits(9, &berths));
    }

    #[test]
    fn the_approximate_root_is_the_idiom() {
        let square = 25.0_f64;
        let root = approximate_sqrt(square as f32, square);
        assert!((root - 5.0).abs() < 0.05, "{root}");
    }
}
