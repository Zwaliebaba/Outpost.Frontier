//! The space paths of TU53 (`Content/SpacePaths.cpp`): the NN path that
//! plans a trip through a system over its trade lanes, and the path
//! helpers over the system records' lanes and objects (work package P3.4d,
//! decision record 0008 point 10).
//!
//! - [`build_nn_path`] (`BuildNnPath`): each lane a leg of kind 0 between
//!   its end rings, the free flight before and after them a leg of kind 1.
//!   GameSupport's best path ([`super::game_support`]) uses it, as do the
//!   guides (P3.6c), the mission NN path (P3.5c) and the random missions
//!   (P3.7).
//! - [`find_entry_ring`], [`is_segment_clear`] (with SpacePop's solar
//!   detection), [`find_obstacle_center`], [`pick_points_around`],
//!   [`is_base_of_affiliation`] and [`fast_length`].
//!
//! The parts that need Content's NPC ships or directives stay with their
//! packages: the guided ships (`RegisterGuidedShip`,
//! `UnregisterGuidedShip`), `IsSameShipOrFormation` and
//! `SetFormationAttitudes` (P3.6c), and `BuildPathDirectives`, whose
//! dock and goto directives are the object lists' (P3.5c).
//!
//! The arithmetic is the C++'s on the x87 stack in its 53-bit mode: `f64`,
//! rounded to `f32` where the C++ stores a float (T11).

// The C++'s `!(x < y)` tests send NaN to the failing side; the port spells
// them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Matrix, Vector, transform_matrix};

use super::patrol_paths::distance_squared_to_segment;
use super::systems::{SystemInfo, TradeLaneRing};
use crate::api;
use crate::server::Cx;

/// A leg of an NN path (`FNnPathLeg`, `MissionObject.h:543-550`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct NnPathLeg {
    /// 0 a lane between two rings, 1 free flight.
    pub kind: i32,
    /// Where the leg starts.
    pub point_a: Vector,
    /// Where it ends.
    pub point_b: Vector,
    /// The ring it starts at (0 for free flight).
    pub object_a: u32,
    /// The ring it ends at.
    pub object_b: u32,
}

impl NnPathLeg {
    /// `MakeFreeLeg`: kind 1 between two points.
    #[must_use]
    pub fn free(a: Vector, b: Vector) -> Self {
        Self {
            kind: 1,
            point_a: a,
            point_b: b,
            object_a: 0,
            object_b: 0,
        }
    }

    /// The lane leg turned around: kind 0, its ends and rings swapped.
    fn reversed(&self) -> Self {
        Self {
            kind: 0,
            point_a: self.point_b,
            point_b: self.point_a,
            object_a: self.object_b,
            object_b: self.object_a,
        }
    }
}

/// `InitEmptyRing`: id 0, at the origin, the identity, reputation -1. The
/// C++ leaves the position of some empty rings unset; it is read only when
/// a ring was found.
fn empty_ring() -> TradeLaneRing {
    TradeLaneRing {
        object: 0,
        position: Vector::ZERO,
        rotation: Matrix::IDENTITY,
        reputation: u32::MAX,
    }
}

/// The inlined subtraction, and `SubtractVectors`: each component a float
/// store.
fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) - f64::from(b.x)) as f32,
        (f64::from(a.y) - f64::from(b.y)) as f32,
        (f64::from(a.z) - f64::from(b.z)) as f32,
    )
}

/// `z*z + y*y`, then `+ x*x`, on the x87 stack.
fn length_squared_zyx(v: Vector) -> f64 {
    let (x, y, z) = (f64::from(v.x), f64::from(v.y), f64::from(v.z));
    (z * z + y * y) + x * x
}

/// The bit guess of the `0x5f3759df` estimate, from the float of the
/// squared length.
fn sqrt_guess(length_squared: f64) -> f64 {
    let stored = length_squared as f32;
    let bits = 0x5f37_59df_i32.wrapping_sub(stored.to_bits().cast_signed() >> 1);
    f64::from(f32::from_bits(bits.cast_unsigned()))
}

/// `FastLength(LengthSquared)`: `((1.5 - y*y*L*0.5) * y) * L` for the bit
/// guess `y`.
#[must_use]
pub fn fast_length(length_squared: f64) -> f64 {
    let guess = sqrt_guess(length_squared);
    let step = 1.5 - (guess * guess * length_squared) * 0.5;
    (step * guess) * length_squared
}

/// The cost form: `((L / divisor) * step) * y`.
fn fast_length_over(length_squared: f64, divisor: f32) -> f64 {
    let guess = sqrt_guess(length_squared);
    let step = 1.5 - (guess * guess * length_squared) * 0.5;
    ((length_squared / f64::from(divisor)) * step) * guess
}

/// `GetLaneRingsOf(System, RingId)`: the rings of the first lane that has
/// the ring (a walk over the lanes, not the ring map).
fn lane_rings_of(record: &SystemInfo, ring: u32) -> Option<&[TradeLaneRing]> {
    record
        .trade_lanes
        .iter()
        .find(|l| l.rings.iter().any(|r| r.object == ring))
        .map(|l| l.rings.as_slice())
}

/// `FindRing(System, &RingId, Out)`: the ring of its lane; `out` is left
/// alone when no lane has it.
fn find_ring(record: &SystemInfo, ring: u32, out: &mut TradeLaneRing) {
    if let Some(found) =
        lane_rings_of(record, ring).and_then(|rings| rings.iter().find(|r| r.object == ring))
    {
        *out = found.clone();
    }
}

/// `FindNearestLaneEnd(Out, System, Position)`: of every lane's first and
/// last ring, the nearest to the position (the first ring's distance
/// `(x*x + z*z) + y*y` kept wide, the last ring's `(z*z + y*y) + x*x`
/// stored as a float; a ring wins while none was taken or when strictly
/// nearer).
fn find_nearest_lane_end(record: &SystemInfo, position: Vector) -> TradeLaneRing {
    let mut best_ring = empty_ring();
    let mut best = -1.0_f32;
    for lane in &record.trade_lanes {
        let (Some(first), Some(last)) = (lane.rings.first(), lane.rings.last()) else {
            continue;
        };
        let d = |r: &TradeLaneRing| {
            (
                f64::from(r.position.x) - f64::from(position.x),
                f64::from(r.position.y) - f64::from(position.y),
                f64::from(r.position.z) - f64::from(position.z),
            )
        };
        let (dx, dy, dz) = d(first);
        let first_distance = (dx * dx + dz * dz) + dy * dy;
        let (lx, ly, lz) = d(last);
        let last_distance = ((lz * lz + ly * ly) + lx * lx) as f32;
        if best == -1.0 || first_distance < f64::from(best) {
            best = first_distance as f32;
            best_ring = first.clone();
        }
        if last_distance < best {
            best_ring = last.clone();
            best = last_distance;
        }
    }
    best_ring
}

/// `FindNearestRing(Out, System, Position)`: over every lane, the ring
/// [`find_entry_ring`] gives without a target; the nearest wins
/// (`(z*z + x*x) + y*y`).
fn find_nearest_ring(record: &SystemInfo, position: Vector) -> TradeLaneRing {
    let mut best_ring = empty_ring();
    let mut best = -1.0_f32;
    for lane in &record.trade_lanes {
        let ring = find_entry_ring(position, &lane.rings, None);
        let dx = f64::from(ring.position.x) - f64::from(position.x);
        let dy = f64::from(ring.position.y) - f64::from(position.y);
        let dz = f64::from(ring.position.z) - f64::from(position.z);
        let distance = (dz * dz + dx * dx) + dy * dy;
        if best == -1.0 || distance < f64::from(best) {
            best = distance as f32;
            best_ring = ring;
        }
    }
    best_ring
}

/// `FindEntryRing(Out, Position, Rings, Target)` (`:607-666`): the ring
/// nearest to the position; with a target, the nearest ring whose plane
/// (its -z axis through its position) separates the position from the
/// target instead, when one has an id.
#[must_use]
pub fn find_entry_ring(
    position: Vector,
    rings: &[TradeLaneRing],
    target: Option<Vector>,
) -> TradeLaneRing {
    let mut nearest = empty_ring();
    let mut nearest_distance = -1.0_f32;
    let mut crossing = empty_ring();
    let mut crossing_distance = -1.0_f32;
    for ring in rings {
        let wide = length_squared_zyx(sub(ring.position, position));
        let distance = wide as f32;
        if wide < f64::from(nearest_distance) || nearest_distance == -1.0 {
            nearest = ring.clone();
            nearest_distance = distance;
        }
        let Some(target) = target else {
            continue;
        };
        let axis = transform_matrix(&ring.rotation, Vector::new(0.0, 0.0, -1.0));
        let (ax, ay, az) = (f64::from(axis.x), f64::from(axis.y), f64::from(axis.z));
        let inverse = 1.0 / ((ax * ax + ay * ay) + az * az).sqrt();
        let (nx, ny, nz) = (ax * inverse, inverse * ay, inverse * az);
        let p = ring.position;
        let offset = -((f64::from(p.z) * nz + f64::from(p.x) * nx) + f64::from(p.y) * ny);
        let side = |v: Vector| {
            ((nz * f64::from(v.z) + ny * f64::from(v.y)) + nx * f64::from(v.x)) + offset
        };
        if !(side(target) * side(position) < 0.0) {
            continue;
        }
        if distance < crossing_distance || crossing_distance == -1.0 {
            crossing = ring.clone();
            crossing_distance = distance;
        }
    }
    if target.is_some() && crossing.object & 0x3fff_ffff != 0 {
        nearest = crossing;
    }
    nearest
}

/// `IsBaseOfAffiliation(&Reputation, &BaseId)`: the vibe's affiliation
/// has a `[BaseFaction]` at the base. False for a base without a record,
/// whose null record the C++ dereferences (D801).
pub fn is_base_of_affiliation(cx: &mut Cx<'_>, vibe: i32, base: u32) -> bool {
    let affiliation = api::reputation::affiliation(cx, vibe).unwrap_or(u32::MAX);
    cx.logic
        .content
        .state
        .bases
        .get(&base)
        .is_some_and(|b| b.find_faction(affiliation).is_some())
}

/// `IsSegmentClear(From, To, System)` (`:466-514`): true while SpacePop's
/// solar detection is off; otherwise false when an object of the system
/// (`ScanObjects` around the segment's middle, radius its length or 2000
/// below a half-length of 1000, at most 32, type mask `0xfdff`) lies closer
/// to the segment than its radius, 600 more for the types with bit 0 or 1.
pub fn is_segment_clear(cx: &mut Cx<'_>, from: Vector, to: Vector, system: u32) -> bool {
    if !super::population::solar_detection(cx) {
        return true;
    }
    let dx = f64::from(to.x) - f64::from(from.x);
    let dy = (f64::from(to.y) - f64::from(from.y)) as f32;
    let dz = (f64::from(to.z) - f64::from(from.z)) as f32;
    let length = ((dx * dx + f64::from(dz) * f64::from(dz)) + f64::from(dy) * f64::from(dy)).sqrt();
    let radius = if length * 0.5 < 1000.0 {
        2000.0
    } else {
        length as f32
    };
    let center = Vector::new(
        (dx * 0.5 + f64::from(from.x)) as f32,
        (f64::from((f64::from(dy) * 0.5) as f32) + f64::from(from.y)) as f32,
        (f64::from((f64::from(dz) * 0.5) as f32) + f64::from(from.z)) as f32,
    );
    let objects =
        api::system::scan_objects(cx, system, 0x20, center, radius, 0xfdff).unwrap_or_default();
    for object in objects {
        let (object_radius, object_center) =
            api::space_obj::radius(cx, object).unwrap_or((0.0, Vector::ZERO));
        let kind = api::space_obj::object_type(cx, object).unwrap_or(0);
        let reach = if kind & 3 != 0 {
            (200.0 + 400.0) + f64::from(object_radius)
        } else {
            f64::from(object_radius)
        };
        let reach_squared = (reach * reach) as f32;
        let (distance, _) = distance_squared_to_segment(object_center, from, to);
        if distance < f64::from(reach_squared) {
            return false;
        }
    }
    true
}

/// `FindObstacleCenter(SystemId, Center, Range, OutCenter)` (`:517-575`):
/// the objects of the system within reach of the centre (`ScanObjects`
/// radius `(1000 + Range) / 2`, at most 32, mask `0xfdff`; an object counts
/// when its centre is closer than half the range plus its radius, 400 more
/// for the types with bit 0 or 1), and the mean of their centres; `None`
/// when there is none (the C++ answers false with the origin).
pub fn find_obstacle_center(
    cx: &mut Cx<'_>,
    system: u32,
    center: Vector,
    range: f32,
) -> Option<Vector> {
    let scan_radius = (((400.0 + 600.0) + f64::from(range)) * 0.5) as f32;
    let objects = api::system::scan_objects(cx, system, 0x20, center, scan_radius, 0xfdff)
        .unwrap_or_default();
    let half_range = (f64::from(range) * 0.5) as f32;
    let mut obstacles = Vec::new();
    for object in objects {
        let (object_radius, object_center) =
            api::space_obj::radius(cx, object).unwrap_or((0.0, Vector::ZERO));
        let kind = api::space_obj::object_type(cx, object).unwrap_or(0);
        let reach = if kind & 3 != 0 {
            400.0 + f64::from(object_radius)
        } else {
            f64::from(object_radius)
        };
        let limit = (f64::from(half_range) + reach) as f32;
        let dx = f64::from(center.x) - f64::from(object_center.x);
        let dy = f64::from(center.y) - f64::from(object_center.y);
        let dz = f64::from(center.z) - f64::from(object_center.z);
        let distance_squared = (dz * dz + dy * dy) + dx * dx;
        if f64::from(limit) * f64::from(limit) > distance_squared {
            obstacles.push(object_center);
        }
    }
    if obstacles.is_empty() {
        return None;
    }
    let inverse = (1.0 / obstacles.len() as f64) as f32;
    let (mut x, mut y, mut z) = (0.0_f64, 0.0_f64, 0.0_f64);
    for o in &obstacles {
        let part_y = (f64::from(inverse) * f64::from(o.y)) as f32;
        let part_z = (f64::from(inverse) * f64::from(o.z)) as f32;
        x += f64::from(inverse) * f64::from(o.x);
        y += f64::from(part_y);
        z += f64::from(part_z);
    }
    Some(Vector::new(x as f32, y as f32, z as f32))
}

/// `PickPointsAround(Center, &MinRange, MaxRange, OutPoints)` (`:578-604`):
/// 16 points around the centre in the xz plane at the angles `i * pi / 8`,
/// each at its own distance `min + rand() / 32768 * (max - min)`.
pub fn pick_points_around(cx: &mut Cx<'_>, center: Vector, min: f32, max: f32) -> [Vector; 16] {
    let mut out = [Vector::ZERO; 16];
    for (index, point) in out.iter_mut().enumerate() {
        let angle = index as f64 * 0.392_699_092_626_571_7;
        let direction = Vector::new(angle.cos() as f32, 0.0, angle.sin() as f32);
        let low = f64::from(min);
        let range =
            f64::from(cx.world.rng().rand()) * 3.051_757_812_5e-05 * (f64::from(max) - low) + low;
        let offset_y = (range * f64::from(direction.y)) as f32;
        let offset_z = (range * f64::from(direction.z)) as f32;
        *point = Vector::new(
            (range * f64::from(direction.x) + f64::from(center.x)) as f32,
            (f64::from(offset_y) + f64::from(center.y)) as f32,
            (f64::from(offset_z) + f64::from(center.z)) as f32,
        );
    }
    out
}

/// The planner of [`build_nn_path`] (`FNnPathPlanner`).
struct Planner<'a> {
    record: &'a SystemInfo,
    /// The candidate routes, each a chain of lane legs.
    routes: Vec<Vec<NnPathLeg>>,
    /// The lanes not yet used, as kind-0 legs.
    lanes: Vec<NnPathLeg>,
    start: Vector,
    end: Vector,
}

/// 11000², the reach between a ring and the next lane's end.
const LANE_REACH_SQUARED: f64 = 1.21e8;

impl Planner<'_> {
    /// `FindExitRing`: the end of the start ring's lane to leave it by:
    /// the other end when the start ring is one, else the end nearer to the
    /// target (the last ring on a tie). `None` without a lane.
    fn find_exit_ring(&self, start_ring: &TradeLaneRing) -> Option<TradeLaneRing> {
        let rings = lane_rings_of(self.record, start_ring.object)?;
        let first = &rings[0];
        let last = &rings[rings.len() - 1];
        let mut chosen = last;
        if first.object != start_ring.object {
            if last.object == start_ring.object {
                chosen = first;
            } else {
                let t = self.end;
                let fx = f64::from(first.position.x) - f64::from(t.x);
                let fy = f64::from(first.position.y) - f64::from(t.y);
                let fz = f64::from(first.position.z) - f64::from(t.z);
                let lx = f64::from(last.position.x) - f64::from(t.x);
                let ly = f64::from(last.position.y) - f64::from(t.y);
                let lz = f64::from((f64::from(last.position.z) - f64::from(t.z)) as f32);
                let first_distance = (fz * fz + fy * fy) + fx * fx;
                let last_distance = (lz * lz + ly * ly) + lx * lx;
                if last_distance > first_distance {
                    chosen = first;
                }
            }
        }
        Some(chosen.clone())
    }

    /// `PickBestRoute`: the route of least summed cost (a float sum); the
    /// ring its last leg ends on goes into `current`.
    fn pick_best_route(&self, current: &mut TradeLaneRing) {
        let mut best = -1.0_f32;
        for route in &self.routes {
            let mut sum = 0.0_f32;
            for leg in route {
                let a_start = sub(leg.point_a, self.start);
                let a_end = sub(leg.point_a, self.end);
                let b_start = sub(leg.point_b, self.start);
                let bx = f64::from(leg.point_b.x) - f64::from(self.end.x);
                let by = f64::from(leg.point_b.y) - f64::from(self.end.y);
                let bz = f64::from(leg.point_b.z) - f64::from(self.end.z);
                let b_end = (bz * bz + by * by) + bx * bx;
                let p = b_end + length_squared_zyx(b_start);
                let q = length_squared_zyx(a_end);
                let r = length_squared_zyx(a_start);
                sum = (p + (q + r) + f64::from(sum)) as f32;
            }
            if sum < best || best < 0.0 {
                best = sum;
                if let Some(last) = route.last() {
                    find_ring(self.record, last.object_b, current);
                }
            }
        }
    }

    /// `RemoveLaneEndingAt`: the first lane leg that ends at the ring.
    fn remove_lane_ending_at(&mut self, ring: &TradeLaneRing) {
        if let Some(i) = self.lanes.iter().position(|l| l.object_b == ring.object) {
            self.lanes.remove(i);
        }
    }

    /// `ExtendRoutes`: the route ending on the ring is taken out (an empty
    /// base when none); each lane with an end within 11000 of the ring
    /// gives a new route, the base and that lane oriented away from it.
    fn extend_routes(&mut self, current: &TradeLaneRing) {
        let mut base = Vec::new();
        if let Some(i) = self
            .routes
            .iter()
            .position(|r| r.last().is_some_and(|l| l.object_b == current.object))
        {
            base = self.routes.remove(i);
        }
        for lane in &self.lanes {
            if length_squared_zyx(sub(lane.point_a, current.position)) <= LANE_REACH_SQUARED {
                let mut route = base.clone();
                route.push(*lane);
                self.routes.push(route);
                continue;
            }
            if length_squared_zyx(sub(lane.point_b, current.position)) <= LANE_REACH_SQUARED {
                let mut route = base.clone();
                route.push(lane.reversed());
                self.routes.push(route);
            }
        }
    }

    /// `CopyRouteEndingAt(Ring, Out, false)`: the first route ending on the
    /// ring.
    fn route_ending_at(&self, ring: &TradeLaneRing) -> Option<Vec<NnPathLeg>> {
        self.routes
            .iter()
            .find(|r| r.last().is_some_and(|l| l.object_b == ring.object))
            .cloned()
    }

    /// `GetRouteCost`: the travel time: the first leg adds the start to its
    /// entry ring at 300 per second, then each leg its length, free flight
    /// at 300 and lanes at 2500, summed as a float.
    fn route_cost(&self, legs: &[NnPathLeg]) -> f64 {
        let mut total = 0.0_f32;
        for (index, leg) in legs.iter().enumerate() {
            if index == 0
                && let Some(rings) = lane_rings_of(self.record, leg.object_a)
                && !rings.is_empty()
            {
                let entry = find_entry_ring(self.start, rings, Some(self.end));
                let to_entry = sub(self.start, entry.position);
                total = (fast_length_over(length_squared_zyx(to_entry), 300.0) + f64::from(total))
                    as f32;
            }
            if leg.kind == 1 {
                let along = sub(leg.point_a, leg.point_b);
                total =
                    (fast_length_over(length_squared_zyx(along), 300.0) + f64::from(total)) as f32;
            } else {
                let dx = f64::from(leg.point_a.x) - f64::from(leg.point_b.x);
                let dy = f64::from(leg.point_a.y) - f64::from(leg.point_b.y);
                let dz = f64::from(leg.point_a.z) - f64::from(leg.point_b.z);
                let length_squared = (dz * dz + dy * dy) + dx * dx;
                total = (fast_length_over(length_squared, 2500.0) + f64::from(total)) as f32;
            }
        }
        f64::from(total)
    }
}

/// The straight flight of [`build_nn_path`]: false (no legs) when the
/// destination is within `range`, else the one free leg.
fn straight(from: Vector, to: Vector, range: f32, legs: &mut Vec<NnPathLeg>) -> bool {
    legs.clear();
    let distance_squared = length_squared_zyx(sub(from, to));
    let range = f64::from(range);
    if !(range * range < distance_squared) {
        return false;
    }
    legs.push(NnPathLeg::free(from, to));
    true
}

/// `BuildNnPath(System, From, To, Legs, Flags, &Range, Unused7)`
/// (`:677-868`): the trip from `from` to `to` through the system record's
/// trade lanes. The flags and the seventh argument are not read.
///
/// Every lane is a candidate leg; the ring nearest to the start
/// ([`find_entry_ring`] over each lane) chooses the first lane and its exit,
/// and the lanes within 11000 of where it is joined start routes too. The
/// cheapest route is extended until it reaches the lane end nearest to the
/// destination, runs out, or comes within 11000 of it; then free flight to
/// the destination follows. When the route's travel time is above the
/// straight flight's, or the system has no lane, the path is the straight
/// flight, or none when the destination is within `range` (false). The
/// first lane is joined at its ring nearest to `from` between `from` and
/// the lane's far end; a lane joined at its exit is dropped.
#[must_use]
pub fn build_nn_path(
    record: &SystemInfo,
    from: Vector,
    to: Vector,
    range: f32,
) -> (bool, Vec<NnPathLeg>) {
    let mut legs = Vec::new();
    let end_ring = find_nearest_lane_end(record, to);
    let mut planner = Planner {
        record,
        routes: Vec::new(),
        lanes: Vec::new(),
        start: from,
        end: to,
    };
    for lane in &record.trade_lanes {
        planner.lanes.push(NnPathLeg {
            kind: 0,
            point_a: lane.start,
            point_b: lane.end,
            object_a: lane.rings.first().map_or(0, |r| r.object),
            object_b: lane.rings.last().map_or(0, |r| r.object),
        });
    }
    let start_ring = find_nearest_ring(record, planner.start);
    let Some(exit_ring) = planner.find_exit_ring(&start_ring) else {
        let path = straight(from, to, range, &mut legs);
        return (path, legs);
    };
    let mut entry_point = Vector::ZERO;
    if let Some(i) = planner
        .lanes
        .iter()
        .position(|l| exit_ring.object == l.object_a || exit_ring.object == l.object_b)
    {
        let lane = planner.lanes.remove(i);
        if exit_ring.object == lane.object_a {
            entry_point = lane.point_b;
            planner.routes.push(vec![lane.reversed()]);
        } else {
            entry_point = lane.point_a;
            planner.routes.push(vec![lane]);
        }
    }
    let mut index = 0;
    while index < planner.lanes.len() {
        let lane = planner.lanes[index];
        if length_squared_zyx(sub(entry_point, lane.point_a)) < LANE_REACH_SQUARED {
            planner.routes.push(vec![lane]);
            planner.lanes.remove(index);
            continue;
        }
        if length_squared_zyx(sub(entry_point, lane.point_b)) < LANE_REACH_SQUARED {
            planner.routes.push(vec![lane.reversed()]);
            planner.lanes.remove(index);
            continue;
        }
        index += 1;
    }
    let mut current = empty_ring();
    planner.pick_best_route(&mut current);
    planner.remove_lane_ending_at(&current.clone());
    let close = |current: &TradeLaneRing| {
        length_squared_zyx(sub(current.position, end_ring.position)) <= LANE_REACH_SQUARED
    };
    while current.object != end_ring.object && !planner.routes.is_empty() && !close(&current) {
        planner.extend_routes(&current.clone());
        planner.pick_best_route(&mut current);
        planner.remove_lane_ending_at(&current.clone());
    }
    if let Some(route) = planner.route_ending_at(&current) {
        legs = route;
    }
    legs.push(NnPathLeg::free(current.position, to));
    let cost = planner.route_cost(&legs) as f32;
    let direct = fast_length(length_squared_zyx(sub(from, to))) / 300.0;
    if f64::from(cost) > direct {
        let path = straight(from, to, range, &mut legs);
        return (path, legs);
    }
    let first = legs[0];
    let rings = lane_rings_of(record, first.object_a).unwrap_or(&[]);
    let entry = find_entry_ring(from, rings, Some(first.point_b));
    legs[0].object_a = entry.object;
    legs[0].point_a = entry.position;
    if legs[0].object_a == legs[0].object_b {
        legs.remove(0);
        if legs.is_empty() {
            legs.push(NnPathLeg::free(from, to));
        }
    }
    (true, legs)
}

/// `BuildBestPathLegs(Unused, System, From, To, Legs, Flags, &Range,
/// Unused7)` (`SpaceSupport.cpp:1354-1377`): [`build_nn_path`]; when it
/// finds no path, the one free leg from `from` to `to`.
#[must_use]
pub fn build_best_path_legs(
    record: &SystemInfo,
    from: Vector,
    to: Vector,
    range: f32,
) -> (bool, Vec<NnPathLeg>) {
    let (path, legs) = build_nn_path(record, from, to, range);
    if path {
        return (path, legs);
    }
    (path, vec![NnPathLeg::free(from, to)])
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::systems::TradeLane;

    fn ring(object: u32, z: f32) -> TradeLaneRing {
        TradeLaneRing {
            object,
            position: Vector::new(0.0, 0.0, z),
            rotation: Matrix::IDENTITY,
            reputation: u32::MAX,
        }
    }

    fn system_with_lane() -> SystemInfo {
        let mut record = SystemInfo::new(1);
        let rings = vec![
            ring(10, 0.0),
            ring(11, -7000.0),
            ring(12, -14000.0),
            ring(13, -21000.0),
        ];
        record.trade_lanes.push(TradeLane {
            start: rings[0].position,
            end: rings[3].position,
            rings,
            ..TradeLane::default()
        });
        record
    }

    #[test]
    fn a_trip_along_the_lane_takes_it() {
        let record = system_with_lane();
        let from = Vector::new(500.0, 0.0, 1000.0);
        let to = Vector::new(0.0, 0.0, -25000.0);
        let (path, legs) = build_nn_path(&record, from, to, 1.0);
        assert!(path);
        assert_eq!(legs.len(), 2);
        assert_eq!(
            (legs[0].kind, legs[0].object_a, legs[0].object_b),
            (0, 10, 13)
        );
        assert_eq!(legs[1].kind, 1);
        assert_eq!(legs[1].point_b, to);
    }

    #[test]
    fn a_short_trip_flies_straight_or_not_at_all() {
        let record = system_with_lane();
        let from = Vector::new(30_000.0, 0.0, 0.0);
        let to = Vector::new(30_000.0, 0.0, 100.0);
        let (path, legs) = build_nn_path(&record, from, to, 1.0);
        assert!(path);
        assert_eq!(legs, [NnPathLeg::free(from, to)]);
        let (path, legs) = build_nn_path(&record, from, to, 500.0);
        assert!(!path);
        assert!(legs.is_empty());
        let (path, legs) = build_best_path_legs(&record, from, to, 500.0);
        assert!(!path);
        assert_eq!(legs, [NnPathLeg::free(from, to)]);
        // No lanes at all.
        let empty = SystemInfo::new(2);
        let (path, legs) = build_nn_path(&empty, from, to, 1.0);
        assert!(path);
        assert_eq!(legs.len(), 1);
    }

    #[test]
    fn the_entry_ring_crosses_towards_the_target() {
        let rings = system_with_lane().trade_lanes[0].rings.clone();
        let near = find_entry_ring(Vector::new(0.0, 0.0, -6000.0), &rings, None);
        assert_eq!(near.object, 11);
        // Towards -z: the nearest ring whose plane lies between.
        let crossing = find_entry_ring(
            Vector::new(0.0, 0.0, -6000.0),
            &rings,
            Some(Vector::new(0.0, 0.0, -30_000.0)),
        );
        assert_eq!(crossing.object, 11);
        let back = find_entry_ring(
            Vector::new(0.0, 0.0, -8000.0),
            &rings,
            Some(Vector::new(0.0, 0.0, 5000.0)),
        );
        assert_eq!(back.object, 11);
        assert!((fast_length(4.0e6) - 2000.0).abs() < 5.0);
    }
}
