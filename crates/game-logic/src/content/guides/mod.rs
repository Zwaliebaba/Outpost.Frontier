//! The NPC guides (TU37, `Content/Guides.cpp`, `GotoGuides.cpp`,
//! `PatrolGuides.cpp`; record 0008 point 7; work package P3.6c): the
//! long-running behaviour of an NPC ship (`FContentNpcShip +0x178`), which
//! gives the ship `[ObjList]` directives and hears its events.
//!
//! **Guides.** A guide is a counted object (`FContentGuide`) in Content's
//! arena ([`GuideRef`]): the NPC ship holds one reference, a guide the
//! ship's formation shares (`PromoteFormationLeader`) another, and
//! `BreakFormationAtDock` gives each NPC member a clone (`Clone`, a
//! member-wise copy that copies the reference count too).
//! [`create_guide`] (`CreateGuide`) builds one from a [`GuideDesc`] by its
//! type. P3.6c ports the guide base, the target guide's targets and flee
//! answer (`FTargetGuide`) and the type 1 guide (`FNothingGuide`, which
//! keeps the ship on a 10000 s delay). The trade (2), patrol (3), wander
//! (4), goto (5) and dock (6) guides are not ported yet: `CreateGuide`
//! builds none of them, so such a ship has no guide (D855).
//!
//! **Directives.** [`execute_directive`] is `Execute` (slot 3) of the
//! directive kinds a guide, the flee answer and the space paths make: the
//! delay, idle, goto, dock and launch directives. A directive that names a
//! target (`FindObjectIdByName`) comes only from a mission's `[ObjList]`,
//! whose run time is P3.5c's; its name finds nothing here. The directives
//! are shared objects ([`super::directives`]).

use std::collections::BTreeMap;

use game_sim::ai::behavior::DirectiveOp;
use game_sim::ai::content::FleeAnswer;
use neuron_core::math::Vector;

use super::directives::{DirectiveList, DirectiveRef};
use super::mission::obj_list::{Delay, DirectiveKind, Dock};
use super::npc_ships::{self, set_goto_destination};
use super::ships::{self, ShipKey};
use crate::api;
use crate::api::ai::BaseOp;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// A guide description (`FGuideDesc`, types 1 to 4; `FMetaBehavior` for 5
/// and 6), as the guide constructors read it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuideDesc {
    /// `Type` (`+0x08`): 0 none, 1 to 6 the guide kinds.
    pub kind: i32,
    /// `TargetId1` (`+0x54`).
    pub target1: u32,
    /// `TargetId2` (`+0x58`).
    pub target2: u32,
    /// `TargetBase1` (`+0x5c`), by base id.
    pub base1: Option<u32>,
    /// `TargetBase2` (`+0x60`), by base id.
    pub base2: Option<u32>,
}

/// A guide in Content's arena (`FContentGuide`, counted).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GuideRef(pub u32);

/// `FTargetGuide`'s two targets (`+0x14` to `+0x20`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Targets {
    /// `TargetId1`.
    pub id1: u32,
    /// `TargetId2`.
    pub id2: u32,
    /// `TargetBase1`.
    pub base1: Option<u32>,
    /// `TargetBase2`.
    pub base2: Option<u32>,
}

/// The guide's class and its members.
#[derive(Clone, Debug, PartialEq)]
pub enum GuideKind {
    /// Type 1, `FNothingGuide` (0x28): the target guide and a 10000 s
    /// delay directive (`+0x24`), one reference held.
    Nothing {
        /// The targets.
        targets: Targets,
        /// `DelayDirective`.
        delay: DirectiveRef,
    },
}

/// `FContentGuide` (the base, `+0x08` to `+0x10`) with its class.
#[derive(Clone, Debug, PartialEq)]
pub struct Guide {
    /// `Ship` (`+0x08`).
    pub ship: Option<NpcShipKey>,
    /// `Type` (`+0x0c`).
    pub kind_id: i32,
    /// `bDone` (`+0x10`).
    pub done: bool,
    /// The class.
    pub kind: GuideKind,
}

/// The arena (`ContentState::guides`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuideArena {
    next: u32,
    entries: BTreeMap<GuideRef, (u32, Guide)>,
}

impl GuideArena {
    fn insert(&mut self, refs: u32, guide: Guide) -> GuideRef {
        self.next += 1;
        let key = GuideRef(self.next);
        self.entries.insert(key, (refs, guide));
        key
    }

    /// The guide.
    #[must_use]
    pub fn get(&self, key: GuideRef) -> Option<&Guide> {
        self.entries.get(&key).map(|(_, g)| g)
    }

    fn get_mut(&mut self, key: GuideRef) -> Option<&mut Guide> {
        self.entries.get_mut(&key).map(|(_, g)| g)
    }

    /// `AddRef`.
    pub fn add_ref(&mut self, key: GuideRef) {
        if let Some(e) = self.entries.get_mut(&key) {
            e.0 += 1;
        }
    }

    /// The reference count (for tests).
    #[must_use]
    pub fn refs(&self, key: GuideRef) -> u32 {
        self.entries.get(&key).map_or(0, |e| e.0)
    }
}

/// `Release`: the last one deletes the guide, which releases its
/// directives.
pub fn release(cx: &mut Cx<'_>, guide: GuideRef) {
    let arena = &mut cx.logic.content.state.guides;
    let Some(e) = arena.entries.get_mut(&guide) else {
        return;
    };
    e.0 = e.0.saturating_sub(1);
    if e.0 != 0 {
        return;
    }
    let Some((_, g)) = arena.entries.remove(&guide) else {
        return;
    };
    match g.kind {
        GuideKind::Nothing { delay, .. } => cx.logic.content.state.directives.release(delay),
    }
}

/// `CreateGuide(Desc, &Guide)` (`Guides.cpp:221-261`): the guide of the
/// description's type for `ship`, with one reference; `None` for a type
/// the port does not build (2 to 6, D855) or any other.
pub fn create_guide(cx: &mut Cx<'_>, desc: &GuideDesc, ship: NpcShipKey) -> Option<GuideRef> {
    let targets = Targets {
        id1: desc.target1,
        id2: desc.target2,
        base1: desc.base1,
        base2: desc.base2,
    };
    let kind = match desc.kind {
        1 => {
            // The delay is made with the directive's own constructor and
            // assigned through `AssignDirective`, which takes a reference:
            // the creating one is never released (two references).
            let arena = &mut cx.logic.content.state.directives;
            let delay = arena.create(DirectiveKind::Delay(Delay {
                priority: 1,
                delay_seconds: 10000.0,
            }));
            arena.add_ref(delay);
            GuideKind::Nothing { targets, delay }
        }
        _ => return None,
    };
    let guide = Guide {
        ship: Some(ship),
        kind_id: desc.kind,
        done: false,
        kind,
    };
    Some(cx.logic.content.state.guides.insert(1, guide))
}

/// The guide part of `FContentNpcShip`'s constructor (`NpcShip.cpp:368-403`):
/// a description of type 0 drops the ship's guide; any other builds one
/// (`CreateGuide`) that starts when the ship has no directives and, with a
/// guide target, when the target's type has a bit of `0xc40`.
pub fn attach_from_params(cx: &mut Cx<'_>, ship: NpcShipKey, desc: &GuideDesc, target: u32) {
    if desc.kind == 0 {
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        return;
    }
    let mut start = true;
    if target != 0 {
        let kind = api::space_obj::object_type(cx, target).unwrap_or(0);
        if kind & 0xc40 == 0 {
            start = false;
        }
    }
    set_guide_from_desc(cx, ship, desc, start);
}

/// `SetGuideFromDesc(Desc, bStart)` (`NpcShip.cpp:900-917`): the old guide
/// (with the AI directive cancelled) replaced by one built from the
/// description, started when the ship has no directives.
pub fn set_guide_from_desc(cx: &mut Cx<'_>, ship: NpcShipKey, desc: &GuideDesc, start: bool) {
    if npc_ships::get(cx, ship).and_then(|s| s.guide).is_some() {
        cancel_ai_directive(cx, ship);
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
    }
    if desc.kind == 0 {
        return;
    }
    let made = create_guide(cx, desc, ship);
    if let Some(s) = npc_ships::get_mut(cx, ship) {
        s.guide = made;
    } else if let Some(g) = made {
        release(cx, g);
        return;
    }
    if start
        && npc_ships::has_no_directives(cx, ShipKey::Npc(ship))
        && let Some(g) = made
    {
        start_guide(cx, g);
    }
}

/// `SetGuide(NewGuide, bStart, bReset)` (`NpcShip.cpp:920-951`): the ship
/// takes the guide (one reference), the guide the ship; reset and started
/// as asked. `None` cancels the AI directive and drops the guide.
pub fn set_guide(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    guide: Option<GuideRef>,
    start: bool,
    reset: bool,
) {
    let Some(new) = guide else {
        if npc_ships::get(cx, ship).and_then(|s| s.guide).is_some() {
            cancel_ai_directive(cx, ship);
        }
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        return;
    };
    let current = npc_ships::get(cx, ship).and_then(|s| s.guide);
    if current != Some(new) {
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        cx.logic.content.state.guides.add_ref(new);
        match npc_ships::get_mut(cx, ship) {
            Some(s) => s.guide = Some(new),
            None => {
                release(cx, new);
                return;
            }
        }
    }
    set_ship(cx, new, Some(ship));
    if reset {
        reset_guide(cx, new);
    }
    if start && npc_ships::has_no_directives(cx, ShipKey::Npc(ship)) {
        start_guide(cx, new);
    }
}

/// `Clone` (slot 3): a member-wise copy, the reference count included;
/// the copy's directives are `AddRef`'ed.
pub fn clone_guide(cx: &mut Cx<'_>, guide: GuideRef) -> Option<GuideRef> {
    let (refs, copy) = cx
        .logic
        .content
        .state
        .guides
        .entries
        .get(&guide)
        .map(|(r, g)| (*r, g.clone()))?;
    match &copy.kind {
        GuideKind::Nothing { delay, .. } => cx.logic.content.state.directives.add_ref(*delay),
    }
    Some(cx.logic.content.state.guides.insert(refs, copy))
}

/// `SubmitDirective(SpaceObjId, DirectiveCancelOp)` on the ship.
fn cancel_ai_directive(cx: &mut Cx<'_>, ship: NpcShipKey) {
    let id = npc_ships::space_obj(cx, ship);
    let _ = api::ai::submit_directive(cx, id, &BaseOp::directive(DirectiveOp::Cancel));
}

/// `IssueOnlyDirective(Ship, Directive)`: the ship's directives cancelled,
/// the directive added, the queue restarted.
fn issue_only_directive(cx: &mut Cx<'_>, ship: NpcShipKey, directive: DirectiveRef) {
    let key = ShipKey::Npc(ship);
    npc_ships::cancel_directives(cx, key);
    npc_ships::add_directive(cx, key, directive);
    npc_ships::restart_directives(cx, key);
}

/// `Start` (slot 4).
pub fn start_guide(cx: &mut Cx<'_>, guide: GuideRef) {
    let Some(g) = cx.logic.content.state.guides.get(guide) else {
        return;
    };
    let ship = g.ship;
    match &g.kind {
        GuideKind::Nothing { delay, .. } => {
            let delay = *delay;
            // The C++ calls on its ship without a null test (D851).
            if let Some(ship) = ship {
                issue_only_directive(cx, ship, delay);
            }
        }
    }
}

/// `Reset` (slot 5): nothing for the target guides.
pub fn reset_guide(cx: &mut Cx<'_>, guide: GuideRef) {
    let _ = (cx, guide);
}

/// `OnDirectivesDone(Word11c, Word118)` (slot 6): the nothing guide starts
/// again.
pub fn on_directives_done(cx: &mut Cx<'_>, guide: GuideRef, result: (i32, i32)) {
    let _ = result;
    let Some(g) = cx.logic.content.state.guides.get(guide) else {
        return;
    };
    match g.kind {
        GuideKind::Nothing { .. } => start_guide(cx, guide),
    }
}

/// `HandleEvent(MsgId, Parms)` (slot 7): the base's does nothing.
pub fn handle_event(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    message: &crate::events::ControllerMessage<'_>,
) {
    let _ = (cx, guide, message);
}

/// `SetShip(Ship)` (slot 8).
pub fn set_ship(cx: &mut Cx<'_>, guide: GuideRef, ship: Option<NpcShipKey>) {
    if let Some(g) = cx.logic.content.state.guides.get_mut(guide) {
        g.ship = ship;
    }
}

/// `IsDone` (slot 9).
#[must_use]
pub fn is_done(cx: &Cx<'_>, guide: GuideRef) -> bool {
    cx.logic
        .content
        .state
        .guides
        .get(guide)
        .is_some_and(|g| g.done)
}

/// `GetType` (slot 10).
#[must_use]
pub fn guide_type(cx: &Cx<'_>, guide: GuideRef) -> i32 {
    cx.logic
        .content
        .state
        .guides
        .get(guide)
        .map_or(0, |g| g.kind_id)
}

/// `OnScanStateChange(bScanning)` (slot 11): the base's does nothing.
pub fn on_scan_state_change(cx: &mut Cx<'_>, guide: GuideRef, scanning: bool) {
    let _ = (cx, guide, scanning);
}

/// `GetTargets` (slot 13): the target guide's two targets.
#[must_use]
pub fn targets(cx: &Cx<'_>, guide: GuideRef) -> Targets {
    match cx.logic.content.state.guides.get(guide).map(|g| &g.kind) {
        Some(GuideKind::Nothing { targets, .. }) => *targets,
        None => Targets::default(),
    }
}

/// The squared distance of `FTargetGuide::RequestFleeDestination`: the
/// float differences, `(z² + y²) + x²` stored as a float.
fn target_distance_sq(ship: Vector, target: Vector) -> f32 {
    let dx = (f64::from(ship.x) - f64::from(target.x)) as f32;
    let dy = (f64::from(ship.y) - f64::from(target.y)) as f32;
    let dz = (f64::from(ship.z) - f64::from(target.z)) as f32;
    ((f64::from(dz) * f64::from(dz) + f64::from(dy) * f64::from(dy))
        + f64::from(dx) * f64::from(dx)) as f32
}

/// `FTargetGuide::RequestFleeDestination` (`Guides.cpp:346-416`) as a
/// value: the answer and whether the targets swap. Both targets are
/// needed; the path to the nearer one (`BuildPathDirectives`, range 2500)
/// gives the answer by its first directive: a goto (type 5) the point
/// within 2000, a dock (0x13) the object to dock at. Taking the first
/// target swaps the targets.
fn target_flee(cx: &Cx<'_>, ship: NpcShipKey, targets: Targets) -> (Option<FleeAnswer>, bool) {
    if targets.id1 == 0 || targets.id2 == 0 {
        return (None, false);
    }
    let position = ships::position(cx, ShipKey::Npc(ship));
    let p1 = api::space_obj::location(cx, targets.id1).map_or(Vector::ZERO, |(p, _)| p);
    let d1 = target_distance_sq(position, p1);
    let p2 = api::space_obj::location(cx, targets.id2).map_or(Vector::ZERO, |(p, _)| p);
    let d2 = target_distance_sq(position, p2);
    let (to, swap) = if d1 < d2 { (p1, true) } else { (p2, false) };
    let legs = npc_ships::system_id(cx, ship);
    let first = cx
        .logic
        .content
        .state
        .systems
        .get(&legs)
        .and_then(|record| {
            let (_, legs) = super::space_paths::build_nn_path(record, position, to, 2500.0);
            legs.into_iter().find(|l| l.kind == 0 || l.kind == 1)
        });
    let Some(leg) = first else {
        return (None, swap);
    };
    let mut answer = FleeAnswer::default();
    if leg.kind == 1 {
        answer.kind = 3;
        answer.position = leg.point_b;
        answer.radius = 2000.0;
    } else {
        answer.kind = 2;
        answer.param = leg.object_a;
    }
    (Some(answer), swap)
}

/// The read-only part of the guide's `RequestFleeDestination` (slot 12),
/// asked before the tick.
#[must_use]
pub fn flee_answer(cx: &Cx<'_>, guide: GuideRef, ship: NpcShipKey) -> Option<FleeAnswer> {
    match cx.logic.content.state.guides.get(guide).map(|g| &g.kind)? {
        GuideKind::Nothing { targets, .. } => target_flee(cx, ship, *targets).0,
    }
}

/// The guide's `RequestFleeDestination` with its effects, after the tick:
/// the target guide swaps its targets when it takes the first.
pub fn request_flee_destination(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    ship: NpcShipKey,
) -> Option<FleeAnswer> {
    let targets = targets(cx, guide);
    let (answer, swap) = match cx.logic.content.state.guides.get(guide).map(|g| &g.kind) {
        Some(GuideKind::Nothing { .. }) => target_flee(cx, ship, targets),
        None => (None, false),
    };
    if swap && let Some(g) = cx.logic.content.state.guides.get_mut(guide) {
        match &mut g.kind {
            GuideKind::Nothing { targets, .. } => {
                std::mem::swap(&mut targets.id1, &mut targets.id2);
                std::mem::swap(&mut targets.base1, &mut targets.base2);
            }
        }
    }
    answer
}

/// `ForwardToGuide(MsgId, Parms)` (`NpcShip.cpp:954-964`): the guide's
/// `HandleEvent`, then a guide that is done is dropped.
pub fn forward_to_guide(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    message: &crate::events::ControllerMessage<'_>,
) {
    let Some(g) = npc_ships::get(cx, ship).and_then(|s| s.guide) else {
        return;
    };
    handle_event(cx, g, message);
    if is_done(cx, g)
        && let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take())
    {
        release(cx, old);
    }
}

/// `BuildPathDirectives(System, From, To, &Directives, Flags, &Range)`
/// (`SpacePaths.cpp:871-899`): one directive per leg of the NN path, a
/// dock at the lane's rings (`SetTarget(ObjectA, ObjectB)`) or a goto to
/// the free leg's end (`SetDestination(PointB, Range, -1, 0, 0, 0)`), each
/// at priority 1, one reference held by the list.
pub fn build_path_directives(
    cx: &mut Cx<'_>,
    system: u32,
    from: Vector,
    to: Vector,
    range: f32,
) -> DirectiveList {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return Vec::new();
    };
    let (_, legs) = super::space_paths::build_nn_path(record, from, to, range);
    let mut out = Vec::new();
    for leg in legs {
        let kind = match leg.kind {
            0 => {
                let mut dock = Dock {
                    priority: 1,
                    ..Dock::default()
                };
                dock.op.dock_object = leg.object_a;
                dock.op.field_10 = leg.object_b;
                DirectiveKind::Dock(dock)
            }
            1 => {
                let mut goto = super::mission::obj_list::Goto::default();
                set_goto_destination(&mut goto, leg.point_b, range, -1.0, 0, false, false);
                goto.priority = 1;
                DirectiveKind::GotoVec(goto)
            }
            _ => continue,
        };
        out.push(cx.logic.content.state.directives.create(kind));
    }
    out
}

// ---- `Execute` (slot 3) ------------------------------------------------------------------------

/// `DefaultPriority(&Op, Ship)`: a priority of 0 takes the ship's (written
/// into the shared directive).
fn default_priority(priority: &mut u32, ship_priority: i32) {
    if *priority == 0 {
        *priority = ship_priority as u32;
    }
}

/// `MuteNpcShip(Ship)`: an NPC ship that is no SpacePop ship and not muted
/// is muted for 10 s.
fn mute_npc_ship(cx: &mut Cx<'_>, ship: ShipKey) {
    let ShipKey::Npc(key) = ship else {
        return;
    };
    let space_pop = cx.logic.content.state.space_pop;
    let Some(s) = npc_ships::get(cx, key) else {
        return;
    };
    if s.is_space_pop_ship(space_pop) || s.muted {
        return;
    }
    npc_ships::start_timer(cx, key, 10.0);
}

/// `FContentShipBase::SubmitDirective(Op)`: `pub::AI::SubmitDirective(...)
/// == 1`.
fn submit(cx: &mut Cx<'_>, ship: ShipKey, priority: u32, op: DirectiveOp) -> bool {
    let id = ships::id(&cx.logic.content.state, ship);
    let mut base = BaseOp::directive(op);
    base.priority = priority;
    api::ai::submit_directive(cx, id, &base) == api::ai::OpRType::ACCEPTED
}

/// `Execute(Ship)` of a directive (slot 3, `MissionObjList.cpp`): false
/// when it finished at once (the ship then advances). The delay, idle,
/// point goto, dock and launch directives; a name to resolve finds nothing
/// (P3.5c), and the kinds only missions give run nothing (P3.5c).
pub fn execute_directive(cx: &mut Cx<'_>, ship: ShipKey, directive: DirectiveRef) -> bool {
    let ship_priority = npc_ships::priority(cx, ship);
    let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
        return false;
    };
    match &mut d.kind {
        DirectiveKind::Delay(delay) => {
            default_priority(&mut delay.priority, ship_priority);
            let (priority, seconds) = (delay.priority, delay.delay_seconds);
            submit(cx, ship, priority, DirectiveOp::Delay(seconds))
        }
        DirectiveKind::Idle(idle) => {
            default_priority(&mut idle.priority, ship_priority);
            let priority = idle.priority;
            submit(cx, ship, priority, DirectiveOp::Idle)
        }
        DirectiveKind::GotoVec(goto)
        | DirectiveKind::GotoSpline(goto)
        | DirectiveKind::GotoShip(goto) => {
            if !goto.target_name.is_empty() || !goto.object_name.is_empty() {
                // `FindTargetId` of a mission's name (P3.5c): a miss
                // finishes the directive.
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            default_priority(&mut goto.priority, ship_priority);
            let (priority, op) = (goto.priority, goto.op.clone());
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Goto(op))
        }
        DirectiveKind::Dock(dock) => {
            if !dock.object_name.is_empty() {
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            default_priority(&mut dock.priority, ship_priority);
            let (priority, op) = (dock.priority, dock.op.clone());
            submit(cx, ship, priority, DirectiveOp::Dock(op))
        }
        DirectiveKind::Launch(launch) => {
            if launch.op.index == -1 || launch.op.launch_object == 0 {
                let id = ships::id(&cx.logic.content.state, ship);
                let Some((target, index, terminal)) =
                    super::dock_queues::ship_docking_target(cx, id)
                else {
                    return false;
                };
                let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
                    return false;
                };
                if let DirectiveKind::Launch(launch) = &mut d.kind {
                    launch.op.launch_object = target;
                    launch.op.index = index;
                    launch.op.terminal = terminal as i32;
                }
            }
            let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
                return false;
            };
            let DirectiveKind::Launch(launch) = &mut d.kind else {
                return false;
            };
            default_priority(&mut launch.priority, ship_priority);
            let (priority, op) = (launch.priority, launch.op.clone());
            let id = ships::id(&cx.logic.content.state, ship);
            super::dock_queues::set_dock_equipment(cx, id, false, false, None);
            submit(cx, ship, priority, DirectiveOp::Launch(op))
        }
        // The other kinds come only from a mission's `[ObjList]` (P3.5c).
        _ => false,
    }
}
