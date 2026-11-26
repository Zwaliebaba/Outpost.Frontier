//! The NPC ship (`FContentNpcShip`, TU43, `Content/NpcShip.cpp`; record
//! 0008 point 7; work package P3.6c) and the NPC ship event list of TU53
//! (`FNpcShipEventList GNpcShipEvents`).
//!
//! **The arena.** Every NPC ship Content makes lives in
//! `ContentState::npc_ships` by its [`NpcShipKey`]; a system record holds
//! the keys of its ships (`FSystemInfo::SpaceObjs`). What the C++ holds by
//! pointer (the event list's records, the guided ships, a formation's
//! members) holds the key and looks the ship up again.
//!
//! **Callbacks.** An NPC ship is the directive callback and the content
//! callback of its own manager (`SetPersonalityParams` with `this`); the
//! port gives it one [`CallbackHandle`] (record 0008 point 4). After the
//! behaviour tick game-sim calls back with the handle
//! ([`super::callbacks`]): a directive result ([`on_directive_complete`])
//! and the reports ([`content_event`]); before the tick, the flee answer
//! ([`flee_answer`]).

use game_sim::ai::content::{CombatEvent, FleeAnswer};
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::GraphType;
use game_sim::data::costume::Costume;
use neuron_core::math::{Matrix, Vector};
use neuron_server::controller::ControllerId;

use super::ContentState;
use super::callbacks::{CallbackHandle, CallbackOwner};
use super::directives::{self, DirectiveList, ShipDirectives};
use super::mission::obj_list::{DirectiveKind, Goto};
use super::players::PlayerShip;
use super::rep_handles::{self, RepHandle};
use super::ships::{self, ShipKey};
use crate::api;
use crate::api::ai::{BaseOp, CallbackArg, Op, SetPersonalityParams};
use crate::api::space_obj::{CargoDesc, CommRequest, ShipInfo};
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

/// `FContentNpcShipParams` (0x504 bytes, `NpcShip.h:37-91`), with the
/// TU50 and TU20 blocks it starts with.
#[derive(Clone, Debug, PartialEq)]
pub struct NpcShipParams {
    /// TU20 `ReputationHandle` (`+0x00`): one reference held by the block.
    pub reputation: Option<RepHandle>,
    /// TU20 `Base` (`+0x04`): the docking base, by id.
    pub base: Option<u32>,
    /// TU20 `Nickname` (`+0x08`, `TString<0x30>`).
    pub nickname: Vec<u8>,
    /// TU20 `System` (`+0x3c`).
    pub system: Option<u32>,
    /// TU20 `HitPoints` (`+0x40`, -1).
    pub hit_points: i32,
    /// TU50 `Formation` (`+0x44`), by nickname id.
    pub formation: Option<u32>,
    /// TU50 `+0x48` (-1): the formation slot.
    pub formation_slot: u32,
    /// TU50 `+0x4c` (-1): the formation id.
    pub formation_id: u32,
    /// TU50 `ClassIds` (`+0x50`).
    pub class_ids: Vec<u32>,
    /// TU50 `Directives` (`+0x60`), one reference held on each.
    pub directives: DirectiveList,
    /// TU50 `ControllerId` (`+0x6c`): the owning controller.
    pub controller: u32,
    /// TU50 `Priority` (`+0x70`).
    pub priority: i32,
    /// TU50 `ShipArchId` (`+0x74`).
    pub archetype: u32,
    /// `+0x78`.
    pub field_0x78: u8,
    /// `Loadout` (`+0x7c`).
    pub loadout: u32,
    /// `Cargo` (`+0x80`).
    pub cargo: Vec<CargoDesc>,
    /// `Orientation` (`+0x90`).
    pub orientation: Matrix,
    /// `Position` (`+0xb4`).
    pub position: Vector,
    /// `StateGraph` (`+0xc0`, -1 for `NOTHING`).
    pub state_graph: i32,
    /// `CommCostume` (`+0xc4`).
    pub costume: Costume,
    /// `Voice` (`+0xf8`).
    pub voice: u32,
    /// `GuideDesc` (`+0xfc`): the guide description (P3.6c's guides).
    pub guide: Option<super::guides::GuideDesc>,
    /// `Pilot` (`+0x100`).
    pub pilot: u32,
    /// `+0x104` (-1): the ship's life (`+0x1bc`), -1 immortal.
    pub life_time: f32,
    /// `+0x108` (24): the life left (`+0x1c8`).
    pub life_left: f32,
    /// `+0x10c` (500): `+0x1c0`.
    pub field_0x10c: f32,
    /// `+0x110` (-1), never read.
    pub field_0x110: i32,
    /// `GuideTargetId` (`+0x114`): with one, the guide starts only when
    /// the object's type has a bit of `0xc40`.
    pub guide_target: u32,
    /// `+0x118`, the ship's `+0x17c`.
    pub field_0x118: Vec<u32>,
    /// `Personality` (`+0x124`).
    pub personality: Personality,
    /// `CreationType` (`+0x4fc`, 2).
    pub creation_type: u32,
    /// `Level` (`+0x500`).
    pub level: i32,
}

impl Default for NpcShipParams {
    /// `FContentNpcShipParams::FContentNpcShipParams` (`NpcShip.cpp:97-118`).
    fn default() -> Self {
        Self {
            reputation: None,
            base: None,
            nickname: Vec::new(),
            system: None,
            hit_points: -1,
            formation: None,
            formation_slot: u32::MAX,
            formation_id: u32::MAX,
            class_ids: Vec::new(),
            directives: Vec::new(),
            controller: 0,
            priority: 0,
            archetype: 0,
            field_0x78: 0,
            loadout: 0,
            cargo: Vec::new(),
            orientation: Matrix::IDENTITY,
            position: Vector::ZERO,
            state_graph: -1,
            costume: Costume::default(),
            voice: 0,
            guide: None,
            pilot: 0,
            life_time: -1.0,
            life_left: 24.0,
            field_0x10c: 500.0,
            field_0x110: -1,
            guide_target: 0,
            field_0x118: Vec::new(),
            personality: Personality::default(),
            creation_type: 2,
            level: 0,
        }
    }
}

/// `FContentNpcShip` (0x5b0 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct NpcShip {
    /// The TU50 ship base (the fields the player ship shares; its three
    /// TU46 fields stay at their defaults).
    pub base: PlayerShip,
    /// TU50 `Directives`, `CurrentDirective`, `Priority`,
    /// `ActiveDirective`.
    pub directives: ShipDirectives,
    /// TU50 `ControllerId` (`+0x170`).
    pub controller: u32,
    /// TU20 `ReputationHandle` (`+0x08`), one reference.
    pub reputation: Option<RepHandle>,
    /// TU20 `Base` (`+0x0c`).
    pub dock_base: Option<u32>,
    /// TU20 `Nickname` (`+0xd4`).
    pub nickname: Vec<u8>,
    /// The ship's callback handle (its `+0x118` and `+0x174`).
    pub callback: CallbackHandle,
    /// `Guide` (`+0x178`).
    pub guide: Option<super::guides::GuideRef>,
    /// `+0x17c`.
    pub field_0x17c: Vec<u32>,
    /// `FleeState` (`+0x188`): 0 ask the guide, 1 a position, 2 and 3 an
    /// object.
    pub flee_state: i32,
    /// `FleeDistance` (`+0x18c`).
    pub flee_distance: f32,
    /// `FleePosition` (`+0x190`).
    pub flee_position: Vector,
    /// `FleeObjectId` (`+0x19c`).
    pub flee_object: u32,
    /// `LastHailPlayer` (`+0x1a0`), by the player's id.
    pub last_hail_player: Option<u32>,
    /// `LastHailTime` (`+0x1a8`).
    pub last_hail_time: f64,
    /// `+0x1b1`: muted; the tether and lead reports say nothing.
    pub muted: bool,
    /// `CreationKind` (`+0x1b4`).
    pub creation_kind: u32,
    /// `CommLinesLeft` (`+0x1b8`, 5).
    pub comm_lines_left: i32,
    /// `+0x1bc`: the ship's life, -1 immortal.
    pub life_time: f32,
    /// `+0x1c0`.
    pub field_0x1c0: f32,
    /// `Loadout` (`+0x1c4`).
    pub loadout: u32,
    /// `+0x1c8`: the life left.
    pub life_left: f32,
    /// `+0x1cc`, the ship's own pilot: cleared by the constructor and
    /// written by nothing in Content; `SubmitNpcPersonality`'s mode 1 reads
    /// it before the archetype's pilot.
    pub field_0x1cc: u32,
    /// `Personality` (`+0x1d0`).
    pub personality: Personality,
    /// `+0x5a8`.
    pub field_0x5a8: u8,
    /// `EncounterParamsId` (`+0x5ac`).
    pub encounter_params: u32,
}

impl NpcShip {
    /// `get_creation_type` (`:762-770`): kind 0 answers 2, 2 answers 0, 1
    /// answers 1, any other 3.
    #[must_use]
    pub fn creation_type(&self) -> u32 {
        match self.creation_kind {
            0 => 2,
            2 => 0,
            1 => 1,
            _ => 3,
        }
    }

    /// `IsSpacePopShip`.
    #[must_use]
    pub fn is_space_pop_ship(&self, space_pop: ControllerId) -> bool {
        self.controller == space_pop.0
    }
}

/// The ship of `key`.
#[must_use]
pub fn get<'a>(cx: &'a Cx<'_>, key: NpcShipKey) -> Option<&'a NpcShip> {
    cx.logic.content.state.npc_ships.get(&key)
}

/// The ship of `key`, to change.
pub fn get_mut<'a>(cx: &'a mut Cx<'_>, key: NpcShipKey) -> Option<&'a mut NpcShip> {
    cx.logic.content.state.npc_ships.get_mut(&key)
}

/// The ship's space object id, 0 when it is gone.
#[must_use]
pub fn space_obj(cx: &Cx<'_>, key: NpcShipKey) -> u32 {
    get(cx, key).map_or(0, |s| s.base.ship)
}

/// The ship's system record id (`GetSystem()->SystemId`); 0 when it has
/// none, where the C++ reads through null.
#[must_use]
pub fn system_id(cx: &Cx<'_>, key: NpcShipKey) -> u32 {
    get(cx, key).and_then(|s| s.base.system).unwrap_or(0)
}

/// `FContentNpcShip::FContentNpcShip(Params)` (`NpcShip.cpp:224-412`): the
/// ship is made in the arena, then its space object (`pub::SpaceObj::Create`
/// with the block's archetype, loadout, hit points, system, placement,
/// controller, base, cargo, costume, vibe, voice and level), its manager's
/// state graph with both callbacks and the personality (`SubmitState`),
/// priority 0, the block's directives and the guide. The ship's own
/// reference on the block's reputation handle is taken.
pub fn construct(cx: &mut Cx<'_>, params: &NpcShipParams) -> NpcShipKey {
    let state = &mut cx.logic.content.state;
    state.next_npc_ship += 1;
    let key = NpcShipKey(state.next_npc_ship);
    let callback = state.callbacks.allocate(CallbackOwner::NpcShip(key));
    if let Some(h) = params.reputation {
        state.rep_handles.add_ref(h);
    }
    let vibe = state.rep_handles.vibe(params.reputation);
    let mut base = PlayerShip::new(0, params.archetype, params.class_ids.clone(), params.system);
    base.formation_slot = params.formation_slot;
    base.formation = params.formation;
    base.formation_id = params.formation_id;
    let ship = NpcShip {
        base,
        directives: ShipDirectives {
            priority: params.priority,
            ..ShipDirectives::default()
        },
        controller: params.controller,
        reputation: params.reputation,
        dock_base: params.base,
        nickname: params.nickname.clone(),
        callback,
        guide: None,
        field_0x17c: params.field_0x118.clone(),
        flee_state: 0,
        flee_distance: 0.0,
        flee_position: Vector::ZERO,
        flee_object: 0,
        last_hail_player: None,
        last_hail_time: -1.0,
        muted: false,
        creation_kind: params.creation_type,
        comm_lines_left: 5,
        life_time: params.life_time,
        field_0x1c0: params.field_0x10c,
        loadout: params.loadout,
        life_left: params.life_left,
        personality: params.personality,
        field_0x1cc: 0,
        field_0x5a8: params.field_0x78,
        encounter_params: 0,
    };
    state.npc_ships.insert(key, ship);

    // `pub::SpaceObj::Create(SpaceObjId, Info)`; the system is not
    // null-tested (D851: 0).
    let mut info = ShipInfo::new(
        params.controller,
        params.system.unwrap_or(0),
        params.archetype,
        params.loadout,
        params.position,
        params.orientation,
    );
    info.hit_pts = params.hit_points;
    info.docking_base = params.base.unwrap_or(0);
    info.cargo = params.cargo.clone();
    info.costume = params.costume.clone();
    info.vibe = vibe;
    info.voice = params.voice;
    info.rank = params.level as u32;
    let id = api::space_obj::create(cx, &info).unwrap_or(0);
    if let Some(s) = get_mut(cx, key) {
        s.base.ship = id;
    }

    // The faction's `scan_for_cargo` list goes into the personality the
    // copy below overwrites at once (`:338-352`): no effect.
    let mut state_graph = params.state_graph;
    if state_graph == -1 {
        state_graph = cx
            .world
            .data()
            .state_graphs
            .find(b"NOTHING", GraphType::Leader);
    }
    let op = SetPersonalityParams {
        state_graph,
        content_callback: CallbackArg::Set(callback),
        directive_callback: CallbackArg::Set(callback),
        set_personality: true,
        personality: params.personality,
    };
    let _ = api::ai::submit_state(cx, id, &BaseOp::new(Op::SetPersonalityParams(Box::new(op))));

    // `SetPriority(&0)`, then the block's directives.
    set_priority(cx, ShipKey::Npc(key), 0);
    set_directives(cx, ShipKey::Npc(key), &params.directives, 0);

    if let Some(desc) = &params.guide {
        super::guides::attach_from_params(cx, key, desc, params.guide_target);
    }
    if let Some(s) = get_mut(cx, key) {
        s.flee_position = Vector::ZERO;
        s.flee_state = 0;
        s.flee_object = 0;
        s.flee_distance = 0.0;
    }
    key
}

/// `FSystemInfo::CreateNpcShip(Params, DockTarget, OutShip)`
/// (`SystemInfo.cpp:758-785`) without a dock target: the ship made from a
/// copy of the block with the record as its system, appended to the
/// record's space objects, and `NpcShipCreated` to every controller. A dock
/// target queues the block for launch instead ([`super::dock_queues`]).
pub fn create_in_system(
    cx: &mut Cx<'_>,
    system: u32,
    params: &NpcShipParams,
    dock_target: u32,
) -> (i32, Option<NpcShipKey>) {
    let mut copy = params.clone();
    copy.system = Some(system);
    // The copy holds its own references.
    copy_params_refs(cx, &copy);
    let result = if dock_target & 0x3fff_ffff != 0 {
        let (queued, ship) = super::dock_queues::queue_dock_launch(cx, &copy, dock_target);
        (i32::from(queued), ship)
    } else {
        let key = construct(cx, &copy);
        if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
            record.space_objs.push(key);
        }
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::NpcShipCreated { ship: key },
        );
        (0, Some(key))
    };
    release_params(cx, copy);
    result
}

/// The references a copy of a parameter block takes (its reputation
/// handle and directives, `FContentNpcShipParams`' copy constructor).
fn copy_params_refs(cx: &mut Cx<'_>, params: &NpcShipParams) {
    let state = &mut cx.logic.content.state;
    if let Some(h) = params.reputation {
        state.rep_handles.add_ref(h);
    }
    for &d in &params.directives {
        state.directives.add_ref(d);
    }
}

/// A parameter block's destructor: its references released.
pub fn release_params(cx: &mut Cx<'_>, params: NpcShipParams) {
    let NpcShipParams {
        reputation,
        mut directives,
        ..
    } = params;
    directives::clear_list(&mut cx.logic.content.state.directives, &mut directives);
    if let Some(h) = reputation {
        rep_handles::release(cx, h);
    }
}

/// `~FContentNpcShip` (`:420-423`) with the TU50 and TU20 parts: the
/// reputation handle released, the guide's reference, the directives and
/// the callback handle (which names nothing afterwards).
pub fn destroy(cx: &mut Cx<'_>, key: NpcShipKey) {
    let Some(ship) = cx.logic.content.state.npc_ships.remove(&key) else {
        return;
    };
    let state = &mut cx.logic.content.state;
    state.callbacks.release(ship.callback);
    if let Some(h) = ship.reputation {
        rep_handles::release(cx, h);
    }
    if let Some(g) = ship.guide {
        super::guides::release(cx, g);
    }
    let arena = &mut cx.logic.content.state.directives;
    let mut list = ship.directives.list;
    directives::clear_list(arena, &mut list);
    if let Some(a) = ship.directives.active {
        arena.release(a);
    }
}

// ---- the TU50 directive queue -----------------------------------------------------------------

/// The ship's directive queue in Content's state: an NPC ship's, or the
/// player's ship object's.
fn state_queue_mut(state: &mut ContentState, ship: ShipKey) -> Option<&mut ShipDirectives> {
    match ship {
        ShipKey::Npc(key) => state.npc_ships.get_mut(&key).map(|s| &mut s.directives),
        ShipKey::Player(player) => state
            .players
            .get_mut(&player)
            .and_then(|p| p.ship.as_mut())
            .map(|s| &mut s.directives),
    }
}

/// The ship's directive queue (an NPC ship's, or the player's ship
/// object's).
fn queue_mut<'a>(cx: &'a mut Cx<'_>, ship: ShipKey) -> Option<&'a mut ShipDirectives> {
    state_queue_mut(&mut cx.logic.content.state, ship)
}

/// The ship's directive queue.
#[must_use]
pub fn queue<'a>(cx: &'a Cx<'_>, ship: ShipKey) -> Option<&'a ShipDirectives> {
    let state = &cx.logic.content.state;
    match ship {
        ShipKey::Npc(key) => state.npc_ships.get(&key).map(|s| &s.directives),
        ShipKey::Player(player) => state
            .players
            .get(&player)
            .and_then(|p| p.ship.as_ref())
            .map(|s| &s.directives),
    }
}

/// The references a ship object's queue holds, released when the object
/// goes (`~FContentShipBase` and its `FMissionDirectiveList` member).
pub fn release_queue(state: &mut ContentState, queue: ShipDirectives) {
    let mut list = queue.list;
    directives::clear_list(&mut state.directives, &mut list);
    if let Some(a) = queue.active {
        state.directives.release(a);
    }
}

/// `SubmitDirective(SpaceObjId, DirectiveCancelOp)`, the result dropped.
fn cancel_ai_directive(cx: &mut Cx<'_>, ship: ShipKey) {
    let id = ships::id(&cx.logic.content.state, ship);
    let op = BaseOp::directive(game_sim::ai::behavior::DirectiveOp::Cancel);
    let _ = api::ai::submit_directive(cx, id, &op);
}

/// The queue's list released and emptied, `CurrentDirective = end()`.
fn drop_queue_list(cx: &mut Cx<'_>, ship: ShipKey) {
    let state = &mut cx.logic.content.state;
    let list = state_queue_mut(state, ship).map(|q| std::mem::take(&mut q.list));
    if let Some(mut list) = list {
        directives::clear_list(&mut state.directives, &mut list);
    }
    if let Some(q) = queue_mut(cx, ship) {
        q.current = None;
    }
}

/// `CancelDirectives` (`BehaviorCmds.cpp:231-243`): the AI directive
/// cancelled, every queued directive released and dropped.
pub fn cancel_directives(cx: &mut Cx<'_>, ship: ShipKey) {
    cancel_ai_directive(cx, ship);
    drop_queue_list(cx, ship);
}

/// `AddDirective(Directive)`: appended, one reference taken.
pub fn add_directive(cx: &mut Cx<'_>, ship: ShipKey, directive: directives::DirectiveRef) {
    if queue_mut(cx, ship).is_none() {
        return;
    }
    cx.logic.content.state.directives.add_ref(directive);
    if let Some(q) = queue_mut(cx, ship) {
        q.list.push(directive);
    }
}

/// `SetDirectives(NewList, Priority)` (`:256-282`): the priority stored
/// (no `set_directive_priority`), the AI directive cancelled, the queue
/// replaced by a copy of `list` (each `AddRef`'ed), then started.
pub fn set_directives(
    cx: &mut Cx<'_>,
    ship: ShipKey,
    list: &[directives::DirectiveRef],
    priority: i32,
) {
    if let Some(q) = queue_mut(cx, ship) {
        q.priority = priority;
    }
    cancel_ai_directive(cx, ship);
    drop_queue_list(cx, ship);
    let copy = directives::copy_list(&mut cx.logic.content.state.directives, list);
    match queue_mut(cx, ship) {
        Some(q) => {
            q.list = copy;
            q.current = None;
        }
        None => {
            let mut copy = copy;
            directives::clear_list(&mut cx.logic.content.state.directives, &mut copy);
            return;
        }
    }
    advance_directive(cx, ship);
}

/// `AdvanceDirective` (`:295-330`): the running entry released and erased,
/// the next one (or, from `end()`, the first) becomes the active directive
/// and runs; one that finishes at once (`Execute` false) advances again.
pub fn advance_directive(cx: &mut Cx<'_>, ship: ShipKey) {
    let next = {
        let state = &mut cx.logic.content.state;
        let Some(q) = state_queue_mut(state, ship) else {
            return;
        };
        if let Some(at) = q.current {
            if at < q.list.len() {
                let gone = q.list.remove(at);
                state.directives.release(gone);
            }
            let Some(q) = state_queue_mut(state, ship) else {
                return;
            };
            if at >= q.list.len() {
                q.current = None;
                return;
            }
            q.list[at]
        } else {
            if q.list.is_empty() {
                return;
            }
            q.current = Some(0);
            q.list[0]
        }
    };
    let state = &mut cx.logic.content.state;
    state.directives.add_ref(next);
    let old = state_queue_mut(state, ship).and_then(|q| q.active.replace(next));
    if let Some(old) = old {
        state.directives.release(old);
    }
    let finished = !super::mission::obj_list_execute::execute(cx, ship, next);
    if finished {
        advance_directive(cx, ship);
    }
}

/// `RestartDirectives`: `CurrentDirective = end()`, then
/// [`advance_directive`].
pub fn restart_directives(cx: &mut Cx<'_>, ship: ShipKey) {
    if let Some(q) = queue_mut(cx, ship) {
        q.current = None;
    }
    advance_directive(cx, ship);
}

/// `HasNoDirectives`.
#[must_use]
pub fn has_no_directives(cx: &Cx<'_>, ship: ShipKey) -> bool {
    queue(cx, ship).is_none_or(|q| q.list.is_empty())
}

/// `SetPriority(&Priority)`: stored, and `set_directive_priority` on the
/// ship.
pub fn set_priority(cx: &mut Cx<'_>, ship: ShipKey, priority: i32) {
    if let Some(q) = queue_mut(cx, ship) {
        q.priority = priority;
    }
    let id = ships::id(&cx.logic.content.state, ship);
    let _ = api::ai::set_directive_priority(cx, id, priority);
}

/// `GetPriority`.
#[must_use]
pub fn priority(cx: &Cx<'_>, ship: ShipKey) -> i32 {
    queue(cx, ship).map_or(0, |q| q.priority)
}

/// `CopyDirectives`: a copy of the queue, each element `AddRef`'ed.
pub fn copy_directives(cx: &mut Cx<'_>, ship: ShipKey) -> DirectiveList {
    let list = queue(cx, ship).map(|q| q.list.clone()).unwrap_or_default();
    directives::copy_list(&mut cx.logic.content.state.directives, &list)
}

/// `FollowDirectivesOf(Leader)` (`BehaviorCmds.cpp:357-399`): the AI
/// directive cancelled, the queue replaced by the leader's very directives
/// (shared, each `AddRef`'ed) and started. A first directive of type 0x13
/// that is a dock is pointed at this ship's own docking terminal when the
/// ship is in the docking list: the write goes into the object the leader
/// shares (an instant trade lane, which answers 0x13 too, only gets its
/// flag byte, D854).
pub fn follow_directives_of(cx: &mut Cx<'_>, ship: ShipKey, leader: ShipKey) {
    cancel_ai_directive(cx, ship);
    drop_queue_list(cx, ship);
    let list = queue(cx, leader)
        .map(|q| q.list.clone())
        .unwrap_or_default();
    let copy = directives::copy_list(&mut cx.logic.content.state.directives, &list);
    match queue_mut(cx, ship) {
        Some(q) => q.list = copy,
        None => {
            let mut copy = copy;
            directives::clear_list(&mut cx.logic.content.state.directives, &mut copy);
            return;
        }
    }
    if let Some(&first) = list.first()
        && cx.logic.content.state.directives.type_code(first) == 0x13
    {
        let id = ships::id(&cx.logic.content.state, ship);
        if let Some((target, index, _)) = super::dock_queues::ship_docking_target(cx, id)
            && let Some(d) = cx.logic.content.state.directives.get_mut(first)
        {
            match &mut d.kind {
                DirectiveKind::Dock(dock) => {
                    dock.op.dock_object = target;
                    dock.op.field_14 = 1;
                    dock.op.port = index;
                }
                DirectiveKind::InstantTradelane(lane) => {
                    // `TradeLaneId` and the low byte of the third word (the
                    // index lands past the object in the C++, D854).
                    lane.op.entrance = target;
                    lane.op.speed = f32::from_bits((lane.op.speed.to_bits() & !0xff) | 1);
                }
                _ => {}
            }
        }
    }
    if let Some(q) = queue_mut(cx, ship) {
        q.current = None;
    }
    advance_directive(cx, ship);
}

/// `FContentNpcShip::OnDirectiveComplete(Word11c, Word118)` (`:466-471`):
/// the TU50 handling first (a non-empty queue advances), then, with the
/// queue empty, the guide's `OnDirectivesDone`.
pub fn on_directive_complete(cx: &mut Cx<'_>, key: NpcShipKey, result: (i32, i32)) {
    let ship = ShipKey::Npc(key);
    if !has_no_directives(cx, ship) {
        advance_directive(cx, ship);
    }
    if has_no_directives(cx, ship)
        && let Some(g) = get(cx, key).and_then(|s| s.guide)
    {
        super::guides::on_directives_done(cx, g, result);
    }
}

// ---- the content callback ---------------------------------------------------------------------

/// A report of the ship's manager (record 0008 point 4), after the tick.
pub fn content_event(cx: &mut Cx<'_>, key: NpcShipKey, event: CombatEvent) {
    match event {
        // `follow_event` keeps `ContentCallback`'s empty body.
        CombatEvent::Follow { .. } => {}
        CombatEvent::Tether {
            own,
            watched,
            status,
            position,
        } => tether_object_event(cx, key, own, watched, status, position),
        CombatEvent::Lead {
            own,
            watched,
            progress,
            value,
            position,
        } => lead_object_event(cx, key, own, watched, progress as u32, value, position),
        CombatEvent::InSights(object) => {
            let ship = space_obj(cx, key);
            let system = system_id(cx, key);
            let to = cx.logic.content.state.space_support;
            let _ = cx.dispatch(
                to,
                &mut ControllerMessage::ShipInSights {
                    ship,
                    object,
                    system,
                },
            );
        }
        CombatEvent::FleeRequested {
            own,
            reason,
            answer,
        } => {
            // The answer the flee took was computed before the tick from the
            // same state (record 0008 point 4); the request makes its effects.
            let _ = answer;
            let _ = request_flee_destination(cx, key, own, reason);
        }
        CombatEvent::ScanState(scanning) => {
            if let Some(g) = get(cx, key).and_then(|s| s.guide) {
                super::guides::on_scan_state_change(cx, g, scanning);
            }
        }
        CombatEvent::State {
            kind,
            owner,
            target,
        } => {
            let system = system_id(cx, key);
            if let Some(s) = get_mut(cx, key) {
                s.base.in_combat = kind == 0;
            }
            let _ = cx.dispatch(
                ControllerId::BROADCAST,
                &mut ControllerMessage::CombatState {
                    state: kind as u32,
                    param1: owner,
                    param2: target,
                    system,
                },
            );
        }
        CombatEvent::Drift => {
            let ship = space_obj(cx, key);
            let system = system_id(cx, key);
            let to = cx.logic.content.state.space_support;
            let _ = cx.dispatch(to, &mut ControllerMessage::CombatDrift { ship, system });
        }
    }
}

/// `PickVoiceLine(Name, "", Voice, &Line)` (TU25, `Gcs.cpp:99-130`) and
/// `GetNicknameId(Line)` on success: `<name>_<nn>` for one of the voice's
/// permutations of the line, drawn with one `rand()` toward the low ones;
/// 0 when the voice has none (no draw).
pub fn voice_line_id(cx: &mut Cx<'_>, name: &[u8], suffix: &[u8], voice: u32) -> u32 {
    match pick_voice_line(cx, name, suffix, voice) {
        Some(line) => api::misc::nickname_id(&line),
        None => 0,
    }
}

/// `PickVoiceLine`: the line's name, `None` when the voice is unknown (-2)
/// or has no permutation of it (-3).
pub fn pick_voice_line(cx: &mut Cx<'_>, name: &[u8], suffix: &[u8], voice: u32) -> Option<Vec<u8>> {
    let tables = cx.logic.content.tables.clone();
    let props = super::tables::voices::find_voice_properties(&tables.voice_properties, voice)?;
    let count = props.permutation_count(api::misc::nickname_id(name));
    if count <= 0 {
        return None;
    }
    let count_f = count as f32;
    let spread =
        (f64::from(cx.world.rng().rand()) * f64::from(1.0_f32 / 32768.0) * 1.570_796_370_506_286_6)
            .cos();
    let pick = (f64::from(count) - spread * f64::from(count_f - 1.0) + 0.5).floor();
    let pick = neuron_core::num::trunc_to_i32(pick);
    let mut line = Vec::new();
    line.extend_from_slice(name);
    line.extend_from_slice(format!("_{pick:02}").as_bytes());
    line.extend_from_slice(suffix);
    // `TString<0x40>`: at most 0x3f characters.
    line.truncate(0x3f);
    Some(line)
}

/// `FSystemInfo::SendComm(Sender, Target, TextId, 0, 0, null, 0.5)`
/// (`SystemInfo.cpp:2019-2037`): one line, interrupting.
pub(crate) fn send_comm(cx: &mut Cx<'_>, sender: u32, target: u32, text: u32) {
    let request = CommRequest {
        sender,
        target,
        voice: 0,
        costume: None,
        comm_type: 0,
        text_ids: vec![text],
        mood: 0,
        duration: 0.5,
        interrupt: true,
    };
    let _ = api::space_obj::send_comm(cx, &request);
}

/// `tether_object_event(Own, Watched, Status, Position)` (`:477-537`):
/// unless the ship is muted, the list's last tether status is this one or
/// the ship has no system: a line of the ship's voice (status 1 and 2 mute
/// the ship for a second, 3 queues the report again in 30 s), spoken to
/// the watched object; then `TetherObject` to the owning controller.
fn tether_object_event(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    own: u32,
    watched: u32,
    status: i32,
    position: Vector,
) {
    let Some(ship) = get(cx, key) else {
        return;
    };
    if ship.muted
        || cx.logic.content.state.npc_ship_events.last_tether_status == status as u32
        || ship.base.system.is_none()
    {
        return;
    }
    let ship_id = ship.base.ship;
    let controller = ship.controller;
    let voice = api::space_obj::voice_id(cx, ship_id).unwrap_or(0);
    let mut line = 0;
    match status {
        1 => {
            line = voice_line_id(cx, b"bc_enter_near_teth", b"", voice);
            start_timer(cx, key, 1.0);
        }
        2 => {
            line = voice_line_id(cx, b"bc_enter_far_teth", b"", voice);
            start_timer(cx, key, 1.0);
        }
        3 => {
            line = voice_line_id(cx, b"bc_exit_far_teth", b"", voice);
            set_comm_line(cx, key, line);
            queue_tether_event(cx, key, watched, 3, position);
        }
        _ => {}
    }
    if line & 0x3fff_ffff != 0 {
        send_comm(cx, ship_id, watched, line);
    }
    let _ = cx.dispatch(
        ControllerId(controller),
        &mut ControllerMessage::TetherObject {
            object: own,
            other: watched,
        },
    );
}

/// `lead_object_event(Own, Watched, Progress, Value, Position)`
/// (`:550-627`): unless the ship is muted, the list's last lead progress is
/// this one or the ship has no system, a line of the ship's voice by the
/// progress (1 and 2 mute the ship for a second, 3 and 6 to 8 queue the
/// report again in 30 s, 4 and 5 only mute); then `LeadObject` to the
/// owning controller.
#[allow(clippy::too_many_arguments)]
fn lead_object_event(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    own: u32,
    watched: u32,
    progress: u32,
    value: f32,
    position: Vector,
) {
    let Some(ship) = get(cx, key) else {
        return;
    };
    let controller = ship.controller;
    let last = cx.logic.content.state.npc_ship_events.last_lead_progress;
    if !ship.muted && last != progress && ship.base.system.is_some() {
        let ship_id = ship.base.ship;
        let voice = api::space_obj::voice_id(cx, ship_id).unwrap_or(0);
        let mut line = 0;
        let mut speak = true;
        match progress {
            1 => {
                line = voice_line_id(cx, b"bc_goto_pos", b"", voice);
                start_timer(cx, key, 1.0);
            }
            2 => {
                if last != 7 {
                    line = voice_line_id(cx, b"bc_goto_flat", b"", voice);
                    start_timer(cx, key, 1.0);
                }
            }
            3 | 6 | 8 => {
                let name: &[u8] = match progress {
                    3 => b"bc_goto_neg",
                    6 => b"bc_follow_pos_app_off",
                    _ => b"bc_follow_neg",
                };
                line = voice_line_id(cx, name, b"", voice);
                set_comm_line(cx, key, line);
                queue_lead_event(cx, key, watched, progress, value, position);
            }
            7 => {
                if last != 2 {
                    line = voice_line_id(cx, b"bc_follow_flat", b"", voice);
                    set_comm_line(cx, key, line);
                    queue_lead_event(cx, key, watched, progress, value, position);
                }
            }
            4 | 5 => {
                start_timer(cx, key, 1.0);
                speak = false;
            }
            _ => {}
        }
        if speak && line & 0x3fff_ffff != 0 {
            send_comm(cx, ship_id, watched, line);
        }
    }
    let _ = cx.dispatch(
        ControllerId(controller),
        &mut ControllerMessage::LeadObject {
            object: own,
            other: watched,
            progress,
            value,
            position,
        },
    );
}

/// The read-only part of `request_flee_destination` (`:641-735`): the
/// flee state's answer, or with state 0 the guide's (record 0008 point 4).
#[must_use]
pub fn flee_answer(cx: &Cx<'_>, key: NpcShipKey) -> Option<FleeAnswer> {
    let ship = get(cx, key)?;
    let mut out = FleeAnswer::default();
    match ship.flee_state {
        0 => {
            let guide = ship.guide?;
            super::guides::flee_answer(cx, guide, key)
        }
        1 => {
            out.kind = 3;
            out.position = ship.flee_position;
            out.radius = ship.flee_distance;
            Some(out)
        }
        2 => {
            out.kind = 1;
            out.param = ship.flee_object;
            out.radius = ship.flee_distance;
            Some(out)
        }
        3 => {
            out.kind = 2;
            out.param = ship.flee_object;
            out.radius = ship.flee_distance;
            Some(out)
        }
        _ => None,
    }
}

/// `request_flee_destination(Own, Reason, ...)` with its effects, after
/// the tick: the answer; with state 0, the guide's own effects and, when it
/// answers, a goto directive (type 5) to the destination becomes the
/// active directive (`SetDestination(&Destination, Radius, -1, 0, 0, 0)`,
/// not run); every path ends with `FleeRequest` to SpaceSupport.
fn request_flee_destination(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    _own: u32,
    reason: i32,
) -> Option<FleeAnswer> {
    let ship = get(cx, key)?;
    let (state, guide) = (ship.flee_state, ship.guide);
    let answer = if state == 0 {
        let answer = guide.and_then(|g| super::guides::request_flee_destination(cx, g, key));
        if let Some(a) = answer {
            let destination = match a.kind {
                1 | 2 => api::space_obj::location(cx, a.param).map_or(Vector::ZERO, |(p, _)| p),
                3 => a.position,
                _ => Vector::ZERO,
            };
            let mut goto = Goto::default();
            set_goto_destination(&mut goto, destination, a.radius, -1.0, 0, false, false);
            let state = &mut cx.logic.content.state;
            let d = state.directives.create(DirectiveKind::GotoVec(goto));
            if let Some(s) = state.npc_ships.get_mut(&key)
                && let Some(old) = s.directives.active.replace(d)
            {
                state.directives.release(old);
            } else {
                // The ship is gone: the local's reference is the only one.
                state.directives.release(d);
            }
        }
        answer
    } else {
        flee_answer(cx, key)
    };
    let ship_id = space_obj(cx, key);
    let system = system_id(cx, key);
    let to = cx.logic.content.state.space_support;
    let _ = cx.dispatch(
        to,
        &mut ControllerMessage::FleeRequest {
            ship: ship_id,
            reason: reason as u32,
            system,
        },
    );
    answer
}

/// `FObjListGotoVec::SetDestination(&Destination, Field50, Field54,
/// ObjectId, bNoCruise, bCruise)` (`MissionObjList.cpp:514-526`): a point
/// goto (kind 1) to the destination, `Field_58` 0, the object only when its
/// low 30 bits are set.
pub fn set_goto_destination(
    goto: &mut Goto,
    destination: Vector,
    field_50: f32,
    field_54: f32,
    object: u32,
    no_cruise: bool,
    cruise: bool,
) {
    goto.op.kind = 1;
    goto.op.destination = destination;
    goto.op.range = field_50;
    goto.op.field_54 = field_54;
    goto.op.field_58 = 0;
    goto.op.cruise = u8::from(cruise);
    if object & 0x3fff_ffff != 0 {
        goto.op.field_5c = object;
    }
    goto.op.no_cruise = u8::from(no_cruise);
}

/// `SetFleePosition(&Position, &Distance)`: flee state 1.
pub fn set_flee_position(cx: &mut Cx<'_>, key: NpcShipKey, position: Vector, distance: f32) {
    if let Some(s) = get_mut(cx, key) {
        s.flee_state = 1;
        s.flee_position = position;
        s.flee_distance = distance;
    }
}

/// `SetFleeObject(&Object, &Distance, bType3)`: flee state 3 (a dock) or 2
/// (an object).
pub fn set_flee_object(cx: &mut Cx<'_>, key: NpcShipKey, object: u32, distance: f32, dock: bool) {
    if let Some(s) = get_mut(cx, key) {
        s.flee_object = object;
        s.flee_distance = distance;
        s.flee_state = if dock { 3 } else { 2 };
    }
}

/// `ClearFlee`.
pub fn clear_flee(cx: &mut Cx<'_>, key: NpcShipKey) {
    if let Some(s) = get_mut(cx, key) {
        s.flee_position = Vector::ZERO;
        s.flee_state = 0;
        s.flee_object = 0;
        s.flee_distance = 0.0;
    }
}

/// `SetField1b1(Value)`: muted; muting stops the ship's event timer.
pub fn set_muted(cx: &mut Cx<'_>, key: NpcShipKey, value: bool) {
    if let Some(s) = get_mut(cx, key) {
        s.muted = value;
    }
    if value {
        clear_timer(cx, key);
    }
}

/// `OnCommLineDone` (`:847-868`): one comm line less; at 0 `Msg0x1006` to
/// the owning controller with a dword of the first queued directive by its
/// type (3 to 5 its `+0x17c`, 7 its `+0x10c`; the guides' type 5 reads
/// their goto part's `Field_64` bits).
pub fn on_comm_line_done(cx: &mut Cx<'_>, key: NpcShipKey) {
    let Some(s) = get_mut(cx, key) else {
        return;
    };
    s.comm_lines_left -= 1;
    if s.comm_lines_left > 0 {
        return;
    }
    let (ship, controller) = (s.base.ship, s.controller);
    let first = s.directives.list.first().copied();
    let word = first
        .and_then(|d| cx.logic.content.state.directives.get(d))
        .map_or(0, |d| directive_word(&d.kind));
    let _ = cx.dispatch(
        ControllerId(controller),
        &mut ControllerMessage::Msg0x1006 {
            ship,
            directive_word: word,
            one: 1,
        },
    );
}

/// The dword `OnCommLineDone` reads from a directive by its type code
/// (`ReadDirectiveDword` at `+0x17c`, `+0x10c` or `+0x1c8`).
fn directive_word(kind: &DirectiveKind) -> u32 {
    match kind {
        DirectiveKind::GotoShip(g) | DirectiveKind::GotoSpline(g) | DirectiveKind::GotoVec(g) => {
            g.field_0x17c as u32
        }
        // The guides' type 5 reads their `+0x17c`: the goto part's
        // `Field_64`.
        DirectiveKind::Guide(g) | DirectiveKind::GuideShip(g) => g.op.goto.field_64.to_bits(),
        DirectiveKind::Follow(f) => f.field_0x10c as u32,
        _ => 0,
    }
}

// ---- TU53's NPC ship event list ---------------------------------------------------------------

/// One record of the event list (`FNpcShipEvent`, 0x30 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct NpcShipEvent {
    /// `Ship` (`+0x00`), matched by its space object id.
    pub ship: NpcShipKey,
    /// The ship's space object id when the record was made.
    pub ship_id: u32,
    /// `CommLineId` (`+0x04`).
    pub comm_line: u32,
    /// `TimerId` (`+0x08`), 0 for none.
    pub timer: u32,
    /// `ObjectId` (`+0x0c`).
    pub object: u32,
    /// `LeadProgress` (`+0x10`).
    pub lead_progress: u32,
    /// `FollowStatus` (`+0x14`), never written but by the list's zeroing.
    pub follow_status: u32,
    /// `TetherStatus` (`+0x18`).
    pub tether_status: u32,
    /// `Value` (`+0x1c`).
    pub value: f32,
    /// `Position` (`+0x20`).
    pub position: Vector,
    /// `Kind` (`+0x2c`): 0 unmute, 1 lead, 2 follow, 3 tether.
    pub kind: i32,
}

/// `FNpcShipEventList` (TU53).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NpcShipEvents {
    /// `Events`.
    pub events: Vec<NpcShipEvent>,
    /// `LastLeadProgress` (`+0x0c`).
    pub last_lead_progress: u32,
    /// `LastFollowStatus` (`+0x10`).
    pub last_follow_status: u32,
    /// `LastTetherStatus` (`+0x14`).
    pub last_tether_status: u32,
}

impl NpcShipEvents {
    /// The first record of the ship (by its space object id).
    fn find(&self, ship_id: u32) -> Option<usize> {
        self.events.iter().position(|e| e.ship_id == ship_id)
    }

    /// A new record of the ship (the fields the C++ leaves unset are 0).
    fn record(ship: NpcShipKey, ship_id: u32) -> NpcShipEvent {
        NpcShipEvent {
            ship,
            ship_id,
            comm_line: 0,
            timer: 0,
            object: 0,
            lead_progress: 0,
            follow_status: 0,
            tether_status: 0,
            value: 0.0,
            position: Vector::ZERO,
            kind: 0,
        }
    }
}

/// `FNpcShipEventList::HandleMessage` (`:961-1033`), SpaceSupport's step 2:
/// a `CommComplete` of a record's line counts the ship's lines down, an
/// `ObjectDestroyed` drops the ship's first record, and a `TimerExpired` of
/// a record's timer reports the event again (or unmutes the ship).
pub fn handle_events_message(cx: &mut Cx<'_>, message: &ControllerMessage<'_>) {
    match message {
        ControllerMessage::CommComplete {
            speaker,
            line,
            result,
            ..
        } => {
            let (speaker, line) = (*speaker, *line);
            let success = *result == crate::events::CommResult::Success;
            let found = cx
                .logic
                .content
                .state
                .npc_ship_events
                .events
                .iter()
                .find(|e| e.ship_id == speaker && e.comm_line == line && success)
                .map(|e| e.ship);
            if let Some(ship) = found {
                on_comm_line_done(cx, ship);
            }
        }
        ControllerMessage::ObjectDestroyed(event) => {
            let list = &mut cx.logic.content.state.npc_ship_events;
            if let Some(at) = list.find(event.id) {
                list.events.remove(at);
            }
        }
        ControllerMessage::TimerExpired { param1, .. } => {
            let timer = *param1;
            let list = &mut cx.logic.content.state.npc_ship_events;
            let Some(at) = list.events.iter().position(|e| e.timer == timer) else {
                return;
            };
            let e = list.events[at];
            match e.kind {
                0 => set_muted(cx, e.ship, false),
                1 => {
                    cx.logic.content.state.npc_ship_events.last_lead_progress = 0;
                    lead_object_event(
                        cx,
                        e.ship,
                        e.ship_id,
                        e.object,
                        e.lead_progress,
                        e.value,
                        e.position,
                    );
                }
                // `follow_event` keeps `ContentCallback`'s empty body.
                2 => cx.logic.content.state.npc_ship_events.last_follow_status = 0,
                3 => {
                    cx.logic.content.state.npc_ship_events.last_tether_status = 0;
                    tether_object_event(
                        cx,
                        e.ship,
                        e.ship_id,
                        e.object,
                        e.tether_status as i32,
                        e.position,
                    );
                }
                _ => return,
            }
            // After the report: a report that queued itself again has its
            // new timer forgotten here, as the C++'s `TimerId = 0` after
            // the call does.
            if let Some(r) = cx.logic.content.state.npc_ship_events.events.get_mut(at) {
                r.timer = 0;
            }
        }
        _ => {}
    }
}

/// `SetCommLine(Ship, CommLineId)`.
pub fn set_comm_line(cx: &mut Cx<'_>, key: NpcShipKey, line: u32) {
    let ship_id = space_obj(cx, key);
    let list = &mut cx.logic.content.state.npc_ship_events;
    if let Some(at) = list.find(ship_id) {
        list.events[at].comm_line = line;
        return;
    }
    let mut e = NpcShipEvents::record(key, ship_id);
    e.comm_line = line;
    list.events.push(e);
}

/// `ClearTimer(Ship)`.
pub fn clear_timer(cx: &mut Cx<'_>, key: NpcShipKey) {
    let ship_id = space_obj(cx, key);
    let list = &mut cx.logic.content.state.npc_ship_events;
    if let Some(at) = list.find(ship_id) {
        list.events[at].timer = 0;
    }
}

/// `StartTimer(Ship, Delay)` (`:1069-1099`): a unique id, the ship muted
/// with five comm lines, the ship's record set to unmute on the timer, the
/// three last statuses cleared, and the timer set on SpaceSupport.
pub fn start_timer(cx: &mut Cx<'_>, key: NpcShipKey, delay: f32) {
    let id = cx.logic.content.state.ids.take();
    set_muted(cx, key, true);
    if let Some(s) = get_mut(cx, key) {
        s.comm_lines_left = 5;
    }
    let ship_id = space_obj(cx, key);
    let list = &mut cx.logic.content.state.npc_ship_events;
    if let Some(at) = list.find(ship_id) {
        list.events[at].timer = id;
        list.events[at].kind = 0;
    } else {
        let mut e = NpcShipEvents::record(key, ship_id);
        e.timer = id;
        list.events.push(e);
    }
    list.last_lead_progress = 0;
    list.last_follow_status = 0;
    list.last_tether_status = 0;
    let to = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, to, id, 0, delay);
}

/// `QueueLeadEvent(Ship, Object, Progress, Value, &Position)`: the report
/// again in 30 s; the list's last lead progress becomes this one.
pub fn queue_lead_event(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    object: u32,
    progress: u32,
    value: f32,
    position: Vector,
) {
    let id = cx.logic.content.state.ids.take();
    let ship_id = space_obj(cx, key);
    let list = &mut cx.logic.content.state.npc_ship_events;
    let at = match list.find(ship_id) {
        Some(at) => at,
        None => {
            list.events.push(NpcShipEvents::record(key, ship_id));
            list.events.len() - 1
        }
    };
    let e = &mut list.events[at];
    e.object = object;
    e.kind = 1;
    e.timer = id;
    e.lead_progress = progress;
    e.value = value;
    e.position = position;
    let to = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, to, id, 0, 30.0);
    cx.logic.content.state.npc_ship_events.last_lead_progress = progress;
}

/// `QueueTetherEvent(Ship, Object, Status, &Position)`: as
/// [`queue_lead_event`] with kind 3.
pub fn queue_tether_event(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    object: u32,
    status: u32,
    position: Vector,
) {
    let id = cx.logic.content.state.ids.take();
    let ship_id = space_obj(cx, key);
    let list = &mut cx.logic.content.state.npc_ship_events;
    let at = match list.find(ship_id) {
        Some(at) => at,
        None => {
            list.events.push(NpcShipEvents::record(key, ship_id));
            list.events.len() - 1
        }
    };
    let e = &mut list.events[at];
    e.kind = 3;
    e.object = object;
    e.timer = id;
    e.tether_status = status;
    e.position = position;
    let to = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, to, id, 0, 30.0);
    cx.logic.content.state.npc_ship_events.last_tether_status = status;
}
