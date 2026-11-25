//! The trade guide (type 2, `FTradeGuide`, `Content/Guides.cpp:459-785`)
//! and its route (`FTradeRoute`, `:459-568`): the ship flies the NN path
//! to its second target (within the target's radius plus 3000), docks
//! there, waits 20 to 30 s, launches, swaps its targets and its two goods,
//! and flies back. Its flee answer docks at the second target or flies
//! toward the lane ring it would join.

// The C++'s `!(x > y)` tests send NaN to the failing side; the port spells
// them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::ai::content::FleeAnswer;
use neuron_core::math::Vector;

use super::super::directives::{self, DirectiveList};
use super::super::mission::obj_list::{Delay, DirectiveKind};
use super::super::npc_ships;
use super::super::ships::{self, ShipKey};
use super::{GuideDesc, Targets, build_path_directives, ship_position, target};
use crate::api;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// `FTradeRoute` (0x20 bytes): the way to the trade target.
#[derive(Clone, Debug, PartialEq)]
pub struct TradeRoute {
    /// `State` (`+0x08`): 0 plan, 1 flying, 2 at a lane, 3 the formation
    /// wait, 4 arrived.
    pub state: i32,
    /// `Center` (`+0x0c`): the target's centre.
    pub center: Vector,
    /// `Range` (`+0x18`): the target's radius plus 3000.
    pub range: f32,
    /// `+0x1c`, 0; no reader.
    pub field_0x1c: u32,
}

impl TradeRoute {
    /// `GetStatus`: 3 arrived, else 1.
    #[must_use]
    pub fn status(&self) -> i32 {
        if self.state == 4 { 3 } else { 1 }
    }
}

/// `FTradeGuide` (type 2, 0x34 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct TradeGuide {
    /// The target guide's two targets.
    pub targets: Targets,
    /// `State` (`+0x24`): 0 the route, 1 dock, 2 docked, 3 launch.
    pub state: i32,
    /// `Route` (`+0x28`).
    pub route: TradeRoute,
    /// `TradeGood` (`+0x2c`), the description's `+0x4c`.
    pub trade_good: u32,
    /// `ReturnGood` (`+0x30`), the description's `+0x50`.
    pub return_good: u32,
}

/// The target's radius and centre (`pub::SpaceObj::GetRadius`; the C++
/// leaves both unset when it fails, 0 here).
fn radius(cx: &Cx<'_>, object: u32) -> (f32, Vector) {
    api::space_obj::radius(cx, object).unwrap_or((0.0, Vector::ZERO))
}

/// `Radius + 3000.0f`, stored as a float.
fn route_range(radius: f32) -> f32 {
    (f64::from(radius) + 3000.0) as f32
}

impl TradeGuide {
    /// `FTradeGuide(Desc)` (`Guides.cpp:574-589`): the route aims at the
    /// second target's `GetRadius` centre, within its radius plus 3000.
    pub(super) fn new(cx: &Cx<'_>, desc: &GuideDesc, targets: Targets) -> Self {
        let (r, center) = radius(cx, targets.id2);
        Self {
            targets,
            state: 0,
            route: TradeRoute {
                state: 0,
                center,
                range: route_range(r),
                field_0x1c: 0,
            },
            trade_good: desc.trade_good,
            return_good: desc.return_good,
        }
    }

    /// `FTradeGuide::SwapTargets` (`:703-733`): the goods and the targets
    /// swapped; the route aims at the new second target's `GetLocation`
    /// position (its `GetRadius` centre is fetched and not used) within its
    /// radius plus 3000, and plans again.
    pub(super) fn swap_targets(&mut self, cx: &Cx<'_>) {
        std::mem::swap(&mut self.trade_good, &mut self.return_good);
        self.targets.swap();
        let position =
            api::space_obj::location(cx, self.targets.id2).map_or(Vector::ZERO, |(p, _)| p);
        let (r, _) = radius(cx, self.targets.id2);
        self.route.range = route_range(r);
        self.route.center = position;
        self.route.state = 0;
    }
}

/// The destination of the ship's active directive when it is a point goto
/// (`GetType() == 5`, read as an `FObjListGotoVec`); else the ship's
/// position. A `Guide` answers 5 too and is read as a goto in the C++
/// (D864); here only a point goto gives its destination.
pub(super) fn active_goto_destination(cx: &Cx<'_>, ship: NpcShipKey) -> Option<Vector> {
    let active = npc_ships::queue(cx, ShipKey::Npc(ship))?.active?;
    match &cx.logic.content.state.directives.get(active)?.kind {
        DirectiveKind::GotoVec(goto) => Some(goto.op.destination),
        _ => None,
    }
}

/// `FTradeRoute::Update(Out)` (`:500-547`): the next directives into a new
/// list (one reference held on each) and the route's status. Planning
/// builds the NN path from the ship (from the active goto's destination
/// once flying) to the centre; no path is the arrival. A lane leg first is
/// state 2, which waits once for the formation (a 1 s delay while another
/// member is in a lane) before planning again.
fn update_route(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    route: &mut TradeRoute,
) -> (i32, DirectiveList) {
    let mut out = DirectiveList::new();
    let Some(ship) = ship else {
        // The C++ reads the null ship's position (D851).
        route.state = 4;
        return (route.status(), out);
    };
    loop {
        match route.state {
            0 | 1 => {
                let from = if route.state == 1 {
                    active_goto_destination(cx, ship).unwrap_or_else(|| ship_position(cx, ship))
                } else {
                    ship_position(cx, ship)
                };
                let system = npc_ships::system_id(cx, ship);
                out = build_path_directives(cx, system, from, route.center, route.range);
                let Some(&first) = out.first() else {
                    route.state = 4;
                    return (route.status(), out);
                };
                match cx.logic.content.state.directives.type_code(first) {
                    5 => route.state = 1,
                    0x13 => route.state = 2,
                    _ if route.state == 0 => route.state = 1,
                    _ => {}
                }
                return (route.status(), out);
            }
            2 => route.state = 3,
            3 => {
                if add_formation_wait_directive(cx, ship, &mut out) {
                    return (route.status(), out);
                }
                route.state = 1;
            }
            // 4, and the C++'s spin above 4 (no state reaches it).
            _ => return (route.status(), out),
        }
    }
}

/// `FTradeRoute::AddFormationWaitDirective(Out)` (`:550-568`): a 1 s delay
/// (priority 1) appended when another member of the ship's formation is in
/// a lane (`HasField120`).
fn add_formation_wait_directive(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    out: &mut DirectiveList,
) -> bool {
    let state = &cx.logic.content.state;
    let own = ships::id(state, ShipKey::Npc(ship));
    let formation = ships::formation_id(state, ShipKey::Npc(ship));
    let waiting = super::super::formations::collect_members(state, formation)
        .into_iter()
        .filter(|&m| ships::id(state, m) != own)
        .any(|m| ships::base(state, m).is_some_and(|b| b.lane_entry != 0));
    if !waiting {
        return false;
    }
    let d = cx
        .logic
        .content
        .state
        .directives
        .create(DirectiveKind::Delay(Delay {
            priority: 1,
            delay_seconds: 1.0,
        }));
    out.push(d);
    true
}

/// `FTradeGuide::Start` (`:607-660`): the route's directives become the
/// ship's (priority 0) until it arrives; then the dock at the second
/// target; once docked a 20 to 30 s delay; then the launch, the targets
/// swapped and the route again. A dock that did not land the ship swaps
/// the targets and plans again. The C++'s second, unused local list makes
/// nothing.
pub(super) fn start(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, g: &mut TradeGuide) {
    loop {
        match g.state {
            0 => {
                let (status, mut next) = update_route(cx, ship, &mut g.route);
                if status == 3 {
                    g.state = 1;
                    continue;
                }
                if let Some(ship) = ship {
                    npc_ships::set_directives(cx, ShipKey::Npc(ship), &next, 0);
                }
                directives::clear_list(&mut cx.logic.content.state.directives, &mut next);
                return;
            }
            1 => {
                target::issue_dock_directive(cx, ship, g.targets.id2);
                g.state = 2;
                return;
            }
            2 => {
                let id = ship.map_or(0, |s| npc_ships::space_obj(cx, s));
                if super::super::dock_queues::ship_docking_target(cx, id).is_some() {
                    target::issue_delay_directive(cx, ship);
                    g.state = 3;
                    return;
                }
                g.swap_targets(cx);
                g.state = 0;
            }
            3 => {
                target::issue_launch_directive(cx, ship);
                g.swap_targets(cx);
                g.state = 0;
            }
            // The C++ spins above 3; no state reaches it.
            _ => return,
        }
    }
}

/// The squared length of `(dx, dy, dz)`: `(dx² + dy²) + dz²`.
fn length_squared_xyz(dx: f32, dy: f32, dz: f64) -> f64 {
    (f64::from(dx) * f64::from(dx) + f64::from(dy) * f64::from(dy)) + dz * dz
}

/// `FTradeGuide::RequestFleeDestination` (`:741-785`): the NN path to the
/// second target (range 1500); none, or free flight first, docks at the
/// target (type 2). A lane first docks at its first ring within 10000;
/// farther away the answer is the unit direction to the ring times 10000
/// (type 3, radius 500; the direction itself, not added to the ship's
/// position).
pub(super) fn flee(cx: &Cx<'_>, ship: Option<NpcShipKey>, g: &TradeGuide) -> Option<FleeAnswer> {
    let ship = ship?;
    let position = ship_position(cx, ship);
    let to = api::space_obj::location(cx, g.targets.id2).map_or(Vector::ZERO, |(p, _)| p);
    let system = npc_ships::system_id(cx, ship);
    let legs = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|record| super::super::space_paths::build_nn_path(record, position, to, 1500.0).1)
        .unwrap_or_default();
    let mut answer = FleeAnswer::default();
    let Some(first) = legs.first().filter(|l| l.kind != 1) else {
        answer.kind = 2;
        answer.param = g.targets.id2;
        return Some(answer);
    };
    let ring = api::space_obj::location(cx, first.object_a).map_or(Vector::ZERO, |(p, _)| p);
    let dz = f64::from(ring.z) - f64::from(position.z);
    let dy = (f64::from(ring.y) - f64::from(position.y)) as f32;
    let dx = (f64::from(ring.x) - f64::from(position.x)) as f32;
    let length_squared = length_squared_xyz(dx, dy, dz);
    if !(length_squared > f64::from(super::LANE_ENTRY_RANGE_SQUARED)) {
        answer.kind = 2;
        answer.param = first.object_a;
        return Some(answer);
    }
    let inverse = (1.0 / length_squared.sqrt()) as f32;
    let nx = (f64::from(dx) * f64::from(inverse)) as f32;
    let ny = (f64::from(dy) * f64::from(inverse)) as f32;
    let nz = dz * f64::from(inverse);
    answer.kind = 3;
    answer.position = Vector::new(
        (f64::from(nx) * 10000.0) as f32,
        (f64::from(ny) * 10000.0) as f32,
        (nz * 10000.0) as f32,
    );
    answer.radius = 500.0;
    Some(answer)
}
