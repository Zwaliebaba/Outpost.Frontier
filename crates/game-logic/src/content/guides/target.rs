//! The target guide (`FTargetGuide`, `Content/Guides.cpp:263-411`): the
//! two targets the guides of types 1 to 4 copy from their description, the
//! three "only directive" helpers they share (a 20 to 30 s delay, a launch,
//! a dock at the second target) and the flee answer of the nothing and
//! wander guides.

use game_sim::ai::content::FleeAnswer;
use neuron_core::math::Vector;

use super::super::mission::obj_list::{Delay, DirectiveKind, Dock, Launch};
use super::super::npc_ships;
use super::super::ships::ShipKey;
use super::{Targets, ship_position};
use crate::api;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// `IssueOnlyDirective(Ship, Directive)`: the ship's directives cancelled,
/// the directive added (one reference), the queue restarted. The C++ calls
/// on its ship without a null test; a guide without a ship issues nothing
/// (D851).
pub(super) fn issue_only_directive(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    directive: super::super::directives::DirectiveRef,
) {
    let Some(ship) = ship else {
        return;
    };
    let key = ShipKey::Npc(ship);
    npc_ships::cancel_directives(cx, key);
    npc_ships::add_directive(cx, key, directive);
    npc_ships::restart_directives(cx, key);
}

/// A new directive in a holding local (one reference), given to the ship
/// as its only directive, then the local's reference released.
pub(super) fn issue_new(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, kind: DirectiveKind) {
    let d = cx.logic.content.state.directives.create(kind);
    issue_only_directive(cx, ship, d);
    cx.logic.content.state.directives.release(d);
}

/// `IssueDelayDirective` (`:296-307`): a delay of `rand() * 11 / 32768 +
/// 20` whole seconds (priority 1) as the ship's only directive.
pub(super) fn issue_delay_directive(cx: &mut Cx<'_>, ship: Option<NpcShipKey>) {
    let seconds = cx.world.rng().rand().wrapping_mul(11) / 32768 + 20;
    issue_new(
        cx,
        ship,
        DirectiveKind::Delay(Delay {
            priority: 1,
            delay_seconds: seconds as f32,
        }),
    );
}

/// `IssueLaunchDirective` (`:311-317`): a launch without a target
/// (priority 1; it finds the ship's docking entry when it runs).
pub(super) fn issue_launch_directive(cx: &mut Cx<'_>, ship: Option<NpcShipKey>) {
    issue_new(
        cx,
        ship,
        DirectiveKind::Launch(Launch {
            priority: 1,
            ..Launch::default()
        }),
    );
}

/// `IssueDockDirective` (`:321-329`): a dock at `target` (`SetTarget(&Id,
/// &0)`, priority 1).
pub(super) fn issue_dock_directive(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, target: u32) {
    let mut dock = Dock {
        priority: 1,
        ..Dock::default()
    };
    dock.op.dock_object = target;
    dock.op.field_10 = 0;
    issue_new(cx, ship, DirectiveKind::Dock(dock));
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

/// `FTargetGuide::RequestFleeDestination` (`Guides.cpp:346-416`): both
/// targets are needed; the path to the nearer one (`BuildPathDirectives`,
/// range 2500) gives the answer by its first directive: a goto (type 5)
/// the point within 2000, a dock (0x13) the object to dock at. Taking the
/// first target swaps the targets (`SwapTargets`).
pub(super) fn flee(
    cx: &Cx<'_>,
    ship: Option<NpcShipKey>,
    targets: &mut Targets,
) -> Option<FleeAnswer> {
    if targets.id1 == 0 || targets.id2 == 0 {
        return None;
    }
    let ship = ship?;
    let position = ship_position(cx, ship);
    let p1 = api::space_obj::location(cx, targets.id1).map_or(Vector::ZERO, |(p, _)| p);
    let d1 = target_distance_sq(position, p1);
    let p2 = api::space_obj::location(cx, targets.id2).map_or(Vector::ZERO, |(p, _)| p);
    let d2 = target_distance_sq(position, p2);
    let to = if d1 < d2 {
        targets.swap();
        p1
    } else {
        p2
    };
    let system = npc_ships::system_id(cx, ship);
    let leg = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|record| {
            let (_, legs) = super::super::space_paths::build_nn_path(record, position, to, 2500.0);
            legs.into_iter().find(|l| l.kind == 0 || l.kind == 1)
        })?;
    let mut answer = FleeAnswer::default();
    if leg.kind == 1 {
        answer.kind = 3;
        answer.position = leg.point_b;
        answer.radius = 2000.0;
    } else {
        answer.kind = 2;
        answer.param = leg.object_a;
    }
    Some(answer)
}
