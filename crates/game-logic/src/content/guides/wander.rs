//! The wander guide (type 4, `FWanderGuide`, `Content/Guides.cpp:787-985`):
//! the ship flies from point to point around itself, inside its zone when
//! it has one, away from obstacles, over clear segments.

// The C++'s `!(x < y)` tests send NaN to the failing side; the port spells
// them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::Vector;

use super::super::directives::DirectiveRef;
use super::super::mission::obj_list::{Delay, DirectiveKind, Goto};
use super::super::npc_ships::{self, set_goto_destination};
use super::super::space_paths;
use super::{GuideDesc, Targets, ship_position, target};
use crate::api;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// `GWanderObstacleRange` (`Guides.cpp:39`): 150.
const OBSTACLE_RANGE: f32 = 150.0;

/// `FWanderGuide` (type 4, 0x3c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct WanderGuide {
    /// The target guide's two targets.
    pub targets: Targets,
    /// `State` (`+0x24`): 1 once started.
    pub state: i32,
    /// `Directive` (`+0x28`): the directive `Start` hands out, one
    /// reference held.
    pub directive: Option<DirectiveRef>,
    /// `Zone` (`+0x2c`): the description's wander zone.
    pub zone: Option<u32>,
    /// `MinRange` (`+0x30`, 1500).
    pub min_range: f32,
    /// `MaxRange` (`+0x34`, 3000).
    pub max_range: f32,
    /// `Height` (`+0x38`, 500): the y of the search centre.
    pub height: f32,
}

impl WanderGuide {
    /// `CreateGuide` case 4 (`:793-797`).
    pub(super) fn new(desc: &GuideDesc, targets: Targets) -> Self {
        Self {
            targets,
            state: 0,
            directive: None,
            zone: desc.wander_zone,
            min_range: 1500.0,
            max_range: 3000.0,
            height: 500.0,
        }
    }
}

/// A point goto as the wander guide makes it: `SetDestination(&Point,
/// Range, -1, 0, false, false)`, then `Field_5A` 0, `Field_5B` 1,
/// `Field_58` 1 and priority 1.
fn wander_goto(destination: Vector, range: f32) -> DirectiveKind {
    let mut goto = Goto::default();
    set_goto_destination(&mut goto, destination, range, -1.0, 0, false, false);
    goto.op.cruise = 0;
    goto.op.no_cruise = 1;
    goto.op.field_58 = 1;
    goto.priority = 1;
    DirectiveKind::GotoVec(goto)
}

/// `AssignDirective(&Directive, New)`: the held directive replaced by a new
/// one (its creating local's reference released, the guide's taken).
fn assign(cx: &mut Cx<'_>, g: &mut WanderGuide, kind: DirectiveKind) {
    let arena = &mut cx.logic.content.state.directives;
    let new = arena.create(kind);
    if let Some(old) = g.directive.replace(new) {
        arena.release(old);
    }
}

/// `v * (1 / |v|)` with the x part unrounded until it is scaled, the y
/// and z parts stored as floats, then each scaled by `scale`: the x and y
/// products and the z product unrounded (`:842-850`, `:931-941`; the
/// callers store z where the C++ does).
fn scaled_direction(v: Vector, length: f64, scale: f64) -> (f32, f32, f64) {
    let inverse = 1.0 / length;
    let x = f64::from(v.x) * inverse;
    let y = (f64::from(v.y) * inverse) as f32;
    let z = (f64::from(v.z) * inverse) as f32;
    (
        (x * scale) as f32,
        (f64::from(y) * scale) as f32,
        f64::from(z) * scale,
    )
}

/// `SubtractVectors`, each component a float store.
fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) - f64::from(b.x)) as f32,
        (f64::from(a.y) - f64::from(b.y)) as f32,
        (f64::from(a.z) - f64::from(b.z)) as f32,
    )
}

/// `FWanderGuide::Start` (`Guides.cpp:833-985`):
/// - an obstacle near the ship (`FindObstacleCenter`, range 150): a goto
///   200 away from its centre (range 100);
/// - else 16 points around `(x, Height, z)` of the ship (`PickPointsAround`
///   with the two ranges), tried from random indices, each once: a point
///   inside the zone (`ClassifyPoint < 1`, or no zone) over a clear segment
///   (`IsSegmentClear`) is a goto there (range 500), and a minimum range
///   below 1500 is reset to 1500 and 3000;
/// - none: while the minimum range is above 100 a 0.5 s delay and both
///   ranges halved; else the ranges reset and, with a zone, a goto toward
///   it (its position within 1000, else 1000 toward it; range 250).
///
/// The held directive (a new one or the last) is the ship's only one.
pub(super) fn start(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, g: &mut WanderGuide) {
    if g.state == 0 {
        g.state = 1;
    }
    // The C++ reads the null ship's position and system (D851).
    let Some(key) = ship else {
        return;
    };
    let position = ship_position(cx, key);
    let system = npc_ships::system_id(cx, key);
    if let Some(obstacle) = space_paths::find_obstacle_center(cx, system, position, OBSTACLE_RANGE)
    {
        let away = sub(position, obstacle);
        let length = ((f64::from(away.x) * f64::from(away.x)
            + f64::from(away.y) * f64::from(away.y))
            + f64::from(away.z) * f64::from(away.z))
        .sqrt();
        let (x, y, z) = scaled_direction(away, length, 200.0);
        let destination = Vector::new(
            (f64::from(position.x) + f64::from(x)) as f32,
            (f64::from(position.y) + f64::from(y)) as f32,
            (f64::from(position.z) + f64::from(z as f32)) as f32,
        );
        assign(cx, g, wander_goto(destination, 100.0));
        if let Some(d) = g.directive {
            target::issue_only_directive(cx, ship, d);
        }
        return;
    }
    let center = Vector::new(position.x, g.height, position.z);
    let points = space_paths::pick_points_around(cx, center, g.min_range, g.max_range);
    let mut used: Vec<i32> = Vec::with_capacity(16);
    let mut found = None;
    for _ in 0..16 {
        let mut index = cx.world.rng().rand().wrapping_mul(16) / 32768;
        while used.contains(&(index & 0xf)) {
            index = index.wrapping_add(1);
        }
        index &= 0xf;
        used.push(index);
        let point = points[index as usize];
        if let Some(zone) = g.zone
            && !(api::zone::classify_point(cx, zone, point) < 1.0)
        {
            continue;
        }
        if space_paths::is_segment_clear(cx, position, point, system) {
            found = Some(point);
            break;
        }
    }
    if let Some(destination) = found {
        assign(cx, g, wander_goto(destination, 500.0));
        if g.min_range < 1500.0 {
            g.max_range = 3000.0;
            g.min_range = 1500.0;
        }
    } else if g.min_range > 100.0 {
        assign(
            cx,
            g,
            DirectiveKind::Delay(Delay {
                priority: 1,
                delay_seconds: 0.5,
            }),
        );
        g.min_range = (f64::from(g.min_range) * 0.5) as f32;
        g.max_range = (f64::from(g.max_range) * 0.5) as f32;
    } else {
        g.max_range = 3000.0;
        g.min_range = 1500.0;
        if let Some(zone) = g.zone {
            let zone_position = api::zone::pos(cx, zone);
            let toward = Vector::new(
                zone_position.x - position.x,
                zone_position.y - position.y,
                zone_position.z - position.z,
            );
            let length_squared = (f64::from(toward.z) * f64::from(toward.z)
                + f64::from(toward.y) * f64::from(toward.y))
                + f64::from(toward.x) * f64::from(toward.x);
            let target = if !(length_squared > 1_000_000.0) {
                zone_position
            } else {
                let (x, y, z) = scaled_direction(toward, length_squared.sqrt(), 1000.0);
                Vector::new(
                    (f64::from(position.x) + f64::from(x)) as f32,
                    (f64::from(y) + f64::from(position.y)) as f32,
                    (z + f64::from(position.z)) as f32,
                )
            };
            assign(cx, g, wander_goto(target, 250.0));
        }
    }
    // The held directive: none only before the first one is made, which
    // the C++ hands to the ship as a null (no path reaches it).
    if let Some(d) = g.directive {
        target::issue_only_directive(cx, ship, d);
    }
}
