//! The placement of an encounter spawn (TU53 `Content/SpawnPlacement.cpp`;
//! record 0008 point 7; work package P3.6d): where a formation the
//! encounters built appears, out of a trade lane (method 1), launched from
//! a dock (2 to 6), cruising in (7) or buzzing the player (8)
//! ([`place_spawn_formation`]), and the targets its leader's guide is
//! given ([`set_leader_guide_targets`]). With them the two TU53 helpers
//! the population shares: the exclusion-sphere test
//! (`TestExclusionSpheres`, `SpaceSupport.cpp:1090-1124`) and the count of
//! NPC ships near a placement (`CountNpcShipsNear`, `DockQueues.cpp:1610-1663`).
//!
//! The x87 keeps intermediate values in extended precision and rounds only
//! where the C++ stores a float; the port evaluates in `f64` and rounds at
//! those stores (decision T11).

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Matrix, Vector, transform_matrix};

use super::guides::{GuideDesc, PatrolPathRef, SpawnPlan};
use super::mission::obj_list::{DirectiveKind, Goto, InstantTradelane};
use super::npc_ships::{self, NpcShipParams};
use super::systems;
use crate::api;
use crate::server::Cx;

/// The population request SpacePop hands to the zones and through them to
/// the encounters (`FSpawnRequest`, `Populators.h:38-71`).
#[derive(Clone, Debug, PartialEq)]
pub struct SpawnRequest {
    /// `bArrival`: the population around a ship that just launched, jumped
    /// in or left a lane; false for the heartbeat's.
    pub arrival: bool,
    /// `ZoneList`: the heartbeat's zone list (0 at the ship, 1 near, 2
    /// ahead); 0 for arrivals.
    pub zone_list: u32,
    /// `ControllerId`: `GSpacePopControllerId`.
    pub controller: u32,
    /// `PlayerShip`: the player whose ship the population is for.
    pub player: u32,
    /// `Placement.Orientation`.
    pub orientation: Matrix,
    /// `Placement.Position`.
    pub position: Vector,
    /// `Velocity`: the ship's linear velocity (zero for none).
    pub velocity: Vector,
    /// `ExclusionSpheres`: a copy of `GSpacePopExclusionSpheres`.
    pub spheres: Vec<[f32; 4]>,
}

/// `2^-15`, `rand()` to `[0, 1)` (`GRandScale`).
const RAND_SCALE: f64 = 1.0 / 32768.0;

/// `GTradeLaneEntryDistance` (5000).
const LANE_ENTRY_DISTANCE: f64 = 5000.0;

/// `GObjectClearanceSq`: 300².
const OBJECT_CLEARANCE_SQ: f64 = 90000.0;

/// `GSegmentEndToleranceSq` (`0.01f`).
const SEGMENT_END_TOLERANCE_SQ: f32 = 0.01;

/// `GDockLaunchDistanceSq`: 3500².
const DOCK_LAUNCH_DISTANCE_SQ: f64 = 12_250_000.0;

/// The `0x5f3759df` estimate of `1/sqrt(value)` (the bits of the float).
fn rsqrt_guess(value: f32) -> f64 {
    let bits = 0x5f37_59df_i32.wrapping_sub(value.to_bits() as i32 >> 1);
    f64::from(f32::from_bits(bits as u32))
}

/// `FastReciprocalSqrt(Value)` (`Populators.cpp:1102-1113`): the estimate
/// refined once, `(1.5 - y * y * value * 0.5) * y`, kept unrounded.
#[must_use]
pub fn fast_reciprocal_sqrt(value: f32) -> f64 {
    let y = rsqrt_guess(value);
    (1.5 - y * y * f64::from(value) * 0.5) * y
}

/// `TestExclusionSpheres(Position, Spheres, &bAll, &bInside)`
/// (`SpaceSupport.cpp:1090-1124`): a point is inside a sphere when its
/// distance (the square in `f64`, its root from the refined estimate of the
/// square stored as a float) is below the radius. True without spheres;
/// with `all`, every sphere must agree (`inside`: all inside, else all
/// outside); without, one must.
#[must_use]
pub fn test_exclusion_spheres(
    position: Vector,
    spheres: Option<&[[f32; 4]]>,
    all: bool,
    inside: bool,
) -> bool {
    let Some(spheres) = spheres else {
        return true;
    };
    let mut inside_count = 0usize;
    let mut outside_count = 0usize;
    for s in spheres {
        let dx = f64::from(position.x) - f64::from(s[0]);
        let dy = f64::from(position.y) - f64::from(s[1]);
        let dz = f64::from(position.z) - f64::from(s[2]);
        let square = dz * dz + dy * dy + dx * dx;
        let y = rsqrt_guess(square as f32);
        let distance = (1.5 - y * y * square * 0.5) * y * square;
        if distance < f64::from(s[3]) {
            inside_count += 1;
        } else {
            outside_count += 1;
        }
    }
    let count = spheres.len();
    match (all, inside) {
        (true, true) => inside_count == count,
        (true, false) => outside_count == count,
        (false, true) => inside_count > 0,
        (false, false) => outside_count > 0,
    }
}

/// `CountNpcShipsNear(System, Placement, &Radius, &ConeRange, ClassId)`
/// (`DockQueues.cpp:1610-1663`): the record's NPC ships (not those whose
/// `+0x5a8` byte is set) within the radius of the placement's position, or
/// with a cone range, within the range and inside the 35° cone around the
/// placement's forward axis; with a class, only the ships of that class.
#[must_use]
pub fn count_npc_ships_near(
    cx: &Cx<'_>,
    system: u32,
    orientation: &Matrix,
    position: Vector,
    radius: f32,
    cone_range: f32,
    class: Option<u32>,
) -> i32 {
    let state = &cx.logic.content.state;
    let Some(record) = state.systems.get(&system) else {
        return 0;
    };
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    let cos_cone = 0.610_865_233_466_029_2_f64.cos();
    let mut count = 0;
    for key in &record.space_objs {
        let Some(ship) = state.npc_ships.get(key) else {
            continue;
        };
        if ship.field_0x5a8 != 0 {
            continue;
        }
        let at = api::space_obj::location(cx, ship.base.ship).map_or(Vector::ZERO, |(p, _)| p);
        let dx = f64::from(position.x) - f64::from(at.x);
        let dy = f64::from(position.y) - f64::from(at.y);
        let dz = f64::from(position.z) - f64::from(at.z);
        let distance_square = dz * dz + dy * dy + dx * dx;
        if !(distance_square <= f64::from(radius_square)) {
            if !(cone_range > 0.0) {
                continue;
            }
            let (vx, vy, vz) = (at.x - position.x, at.y - position.y, at.z - position.z);
            let v_square = (f64::from(vz) * f64::from(vz)
                + f64::from(vy) * f64::from(vy)
                + f64::from(vx) * f64::from(vx)) as f32;
            if !(fast_reciprocal_sqrt(v_square) * f64::from(v_square) <= f64::from(cone_range)) {
                continue;
            }
            let m = &orientation.m;
            let (fx, fy, fz) = (-m[0][2], -m[1][2], -m[2][2]);
            let f_square = (f64::from(fz) * f64::from(fz)
                + f64::from(fy) * f64::from(fy)
                + f64::from(fx) * f64::from(fx)) as f32;
            let dot = (f64::from(vz) * f64::from(fz)
                + f64::from(vy) * f64::from(fy)
                + f64::from(fx) * f64::from(vx)) as f32;
            let v_length = (fast_reciprocal_sqrt(v_square) * f64::from(v_square)) as f32;
            let f_length = fast_reciprocal_sqrt(f_square) * f64::from(f_square);
            let cosine = f64::from(dot) / (f_length * f64::from(v_length));
            if !(cos_cone <= cosine) {
                continue;
            }
        }
        if let Some(class) = class
            && !ship.base.class_ids.contains(&class)
        {
            continue;
        }
        count += 1;
    }
    count
}

/// `RandIndex(Count)`: `rand() * count / 32768`.
fn rand_index(cx: &mut Cx<'_>, count: usize) -> usize {
    usize::try_from(cx.world.rng().rand().wrapping_mul(count as i32) / 32768).unwrap_or(0)
}

/// `RandCoin`: `rand() * 2 / 32768`, 0 or 1.
fn rand_coin(cx: &mut Cx<'_>) -> i32 {
    cx.world.rng().rand().wrapping_mul(2) / 32768
}

fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x - b.x, a.y - b.y, a.z - b.z)
}

fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

fn scale(v: Vector, factor: f32) -> Vector {
    Vector::new(factor * v.x, factor * v.y, factor * v.z)
}

/// `VectorLength`: `sqrt((x² + y²) + z²)` unrounded.
fn length(v: Vector) -> f64 {
    ((f64::from(v.x) * f64::from(v.x) + f64::from(v.y) * f64::from(v.y))
        + f64::from(v.z) * f64::from(v.z))
    .sqrt()
}

/// Each component of `v` times an unrounded factor, stored.
fn scaled(v: Vector, factor: f64) -> Vector {
    Vector::new(
        (f64::from(v.x) * factor) as f32,
        (f64::from(v.y) * factor) as f32,
        (f64::from(v.z) * factor) as f32,
    )
}

/// `AddOtherPlayerSpheres`: a sphere of 2500 around the ship of each other
/// player of the record, in the record's order.
fn add_other_player_spheres(
    cx: &Cx<'_>,
    system: u32,
    request: &SpawnRequest,
    out: &mut Vec<[f32; 4]>,
) {
    let own = cx
        .logic
        .content
        .state
        .players
        .get(&request.player)
        .map_or(0, super::players::PlayerState::ship_id);
    let state = &cx.logic.content.state;
    let Some(record) = state.systems.get(&system) else {
        return;
    };
    for p in &record.players {
        let Some(ship) = state.players.get(p).and_then(|s| s.ship.as_ref()) else {
            continue;
        };
        if ship.ship == own {
            continue;
        }
        let at = api::space_obj::location(cx, ship.ship).map_or(Vector::ZERO, |(v, _)| v);
        out.push([at.x, at.y, at.z, 2500.0]);
    }
}

/// `AddLeaderDirective`: the directive in front of the leader's list, one
/// reference held by the block.
fn add_leader_directive(cx: &mut Cx<'_>, leader: &mut NpcShipParams, kind: DirectiveKind) {
    let arena = &mut cx.logic.content.state.directives;
    let d = arena.create(kind);
    leader.directives.insert(0, d);
    arena.add_ref(d);
    arena.release(d);
}

/// `IsClearOfObjects(&Position, System)` (`:164-190`): no object of a scan
/// of 300 (type `0x7f0000`, 64 at most) is nearer than its radius plus 300
/// (its squared radius, stored, plus 90000 above the squared distance).
fn is_clear_of_objects(cx: &mut Cx<'_>, position: Vector, system: u32) -> bool {
    let ids =
        api::system::scan_objects(cx, system, 0x40, position, 300.0, 0x7f_0000).unwrap_or_default();
    for id in ids {
        let Ok((radius, center)) = api::space_obj::radius(cx, id) else {
            continue;
        };
        let radius_sq = radius * radius;
        let dx = f64::from(position.x) - f64::from(center.x);
        let dy = f64::from(position.y) - f64::from(center.y);
        let dz = f64::from(position.z) - f64::from(center.z);
        let distance_sq = (dz * dz + dx * dx) + dy * dy;
        if f64::from(radius_sq) + OBJECT_CLEARANCE_SQ > distance_sq {
            return false;
        }
    }
    true
}

/// Whether a scan of 200 (type `0x7ffdff`, 32 at most) around the point
/// finds an object, with the solar detection on; false without it.
fn objects_near(cx: &mut Cx<'_>, position: Vector, system: u32) -> bool {
    if !super::population::solar_detection(cx) {
        return false;
    }
    !api::system::scan_objects(cx, system, 0x20, position, 200.0, 0x7f_fdff)
        .unwrap_or_default()
        .is_empty()
}

/// `GetSpherePoints(&Center, &Radius, Out)` (`:192-226`): the center plus
/// the radius times the 256 unit directions of a 16 by 16 polar grid
/// (polar angle `i * pi/16`, azimuth `j * pi/8`; entry `j * 16 + i`).
fn sphere_points(center: Vector, radius: f32) -> Vec<Vector> {
    let mut directions = vec![Vector::ZERO; 256];
    for polar in 0..16_usize {
        let polar_angle = polar as f64 * f64::from(0.196_349_546_313_285_83_f32);
        let sin_polar = polar_angle.sin();
        let cos_polar = polar_angle.cos() as f32;
        for azimuth in 0..16_usize {
            let azimuth_angle = azimuth as f64 * 0.392_699_092_626_571_66;
            directions[azimuth * 16 + polar] = Vector::new(
                (azimuth_angle.cos() * sin_polar) as f32,
                (azimuth_angle.sin() * sin_polar) as f32,
                cos_polar,
            );
        }
    }
    let r = f64::from(radius);
    directions
        .iter()
        .map(|d| {
            let y = (r * f64::from(d.y)) as f32;
            let z = (r * f64::from(d.z)) as f32;
            Vector::new(
                (r * f64::from(d.x) + f64::from(center.x)) as f32,
                y + center.y,
                z + center.z,
            )
        })
        .collect()
}

/// `IsAtSegmentEnd`: every axis within the squared tolerance.
fn is_at_segment_end(point: Vector, end: Vector) -> bool {
    let dx = f64::from(point.x) - f64::from(end.x);
    let dy = f64::from(point.y) - f64::from(end.y);
    let dz = f64::from(point.z) - f64::from(end.z);
    let tolerance = f64::from(SEGMENT_END_TOLERANCE_SQ);
    dx * dx <= tolerance && dy * dy <= tolerance && dz * dz <= tolerance
}

/// Whether the point is within `max_distance` of every zone (-1 turns the
/// test off).
fn within_zones(cx: &mut Cx<'_>, zones: &[u32], max_distance: f32, point: Vector) -> bool {
    for &zone in zones {
        if max_distance == -1.0 {
            continue;
        }
        if api::zone::distance(cx, zone, point) > max_distance {
            return false;
        }
    }
    true
}

/// A segment spawn point's outputs: the point, the end walked towards and
/// the direction (1 or -1).
struct SegmentPoint {
    position: Vector,
    target: Vector,
    direction: i32,
}

/// `FindSegmentSpawnPoint(...)` (`:245-370`) without inside spheres or
/// zones to avoid: from the segment point nearest to the position, up to
/// 15 points 200 apart towards an end chosen at random; the second and
/// third pass walk the other way. A point must be outside every sphere,
/// within the distance of every zone, have no object within 200 (with the
/// solar detection) and be clear of objects.
#[allow(clippy::too_many_arguments)]
fn find_segment_spawn_point(
    cx: &mut Cx<'_>,
    spheres: &[[f32; 4]],
    zones: &[u32],
    max_zone_distance: f32,
    position: Vector,
    start: Vector,
    end: Vector,
    system: u32,
) -> Option<SegmentPoint> {
    let (_, closest) = super::patrol_paths::distance_squared_to_segment(position, start, end);
    let delta = sub(end, start);
    let inv_length = 1.0 / length(delta);
    let mut step = scaled(delta, inv_length);
    let mut out = SegmentPoint {
        position: Vector::ZERO,
        target: Vector::ZERO,
        direction: 0,
    };
    let far;
    if rand_coin(cx) != 0 {
        step = scale(step, 200.0);
        out.direction = -1;
        out.target = start;
        far = end;
    } else {
        step = scale(step, -200.0);
        out.direction = 1;
        out.target = end;
        far = start;
    }
    for pass in 0..=2 {
        if pass == 1 {
            out.target = far;
            step = scale(step, -1.0);
            out.direction = -out.direction;
        }
        for index in 0..15 {
            let offset = scaled(step, f64::from(index));
            out.position = add(offset, closest);
            let (_, on_segment) =
                super::patrol_paths::distance_squared_to_segment(out.position, start, end);
            if is_at_segment_end(on_segment, start) || is_at_segment_end(on_segment, end) {
                break;
            }
            if !test_exclusion_spheres(out.position, Some(spheres), true, false) {
                continue;
            }
            if !within_zones(cx, zones, max_zone_distance, out.position) {
                continue;
            }
            if objects_near(cx, out.position, system) {
                continue;
            }
            if is_clear_of_objects(cx, out.position, system) {
                return Some(out);
            }
        }
    }
    None
}

/// `random_inside_cylinder(Radius, HalfHeight)` (`Common/common_misc.cpp:562-579`):
/// three draws; the radius `sqrt(u1) * radius` (stored), the height
/// `(h - (-h)) * u2 + (-h)` and `phi = u3 * 2 pi`; the result
/// `(sin(phi) R, height, cos(phi) R)` around the y axis.
fn random_inside_cylinder(cx: &mut Cx<'_>, radius: f32, half_height: f32) -> Vector {
    const UNIT: f64 = 3.051_757_812_5e-5;
    const TWO_PI: f64 = 6.283_185_482_025_146_5;
    let rng = cx.world.rng();
    let u1 = f64::from(rng.rand()) * UNIT;
    let scaled = (u1.sqrt() * f64::from(radius)) as f32;
    let neg_half = -half_height;
    let t = f64::from(rng.rand()) * UNIT;
    let phi = f64::from(rng.rand()) * UNIT * TWO_PI;
    Vector::new(
        (phi.sin() * f64::from(scaled)) as f32,
        ((f64::from(half_height) - f64::from(neg_half)) * t + f64::from(neg_half)) as f32,
        (phi.cos() * f64::from(scaled)) as f32,
    )
}

/// `FindSphereSpawnPoint(...)` (`:372-481`) without inside spheres (the
/// zones to avoid are queried by the C++ but not used, so the port leaves
/// them out): the points of the sphere around the center
/// ([`sphere_points`]) whose height is within half the cylinder's height
/// of the look point's and that are outside every sphere, drawn at random
/// (`floor(rand() * 2^-15 * (n - 1) + 0.5)`); a point must be within the
/// distance of every zone, then two attempts look for a start (the look
/// point plus a random point of the cylinder, at the point's height) with
/// no object within 200 (up to four draws; a fourth fails), a clear
/// segment to the point, and the point clear of objects. The point and the
/// start.
#[allow(clippy::too_many_arguments)]
fn find_sphere_spawn_point(
    cx: &mut Cx<'_>,
    spheres: &[[f32; 4]],
    zones: &[u32],
    max_zone_distance: f32,
    center: Vector,
    radius: f32,
    look_from: Vector,
    cylinder_radius: f32,
    cylinder_height: f32,
    system: u32,
) -> Option<(Vector, Vector)> {
    let points = sphere_points(center, radius);
    let mut candidates: Vec<usize> = Vec::with_capacity(256);
    let half_height = f64::from(cylinder_height) * 0.5;
    let max_height = f64::from(look_from.y) + half_height;
    for polar in 0..16 {
        for azimuth in 0..16 {
            let at = polar * 16 + azimuth;
            let p = points[at];
            let mut fits = true;
            if f64::from(p.y) > max_height || f64::from(look_from.y) - half_height > f64::from(p.y)
            {
                fits = false;
            }
            if test_exclusion_spheres(p, Some(spheres), true, false) && fits {
                candidates.push(at);
            }
        }
    }
    while !candidates.is_empty() {
        let scaled =
            (f64::from(cx.world.rng().rand()) * RAND_SCALE * (candidates.len() as i64 - 1) as f64
                + 0.5)
                .floor();
        let pick = (scaled as i32) as usize;
        let at = candidates[pick];
        let position = points[at];
        candidates.remove(pick);
        if !within_zones(cx, zones, max_zone_distance, position) {
            continue;
        }
        for _attempt in 1..3 {
            let mut tries = 0;
            let mut from;
            let mut clear;
            loop {
                let offset = random_inside_cylinder(cx, cylinder_radius, cylinder_height);
                from = Vector::new(look_from.x + offset.x, position.y, look_from.z + offset.z);
                tries += 1;
                clear = !objects_near(cx, from, system);
                if clear || tries > 3 {
                    break;
                }
            }
            if clear
                && tries <= 3
                && super::space_paths::is_segment_clear(cx, from, position, system)
                && is_clear_of_objects(cx, position, system)
            {
                return Some((position, from));
            }
        }
    }
    None
}

/// `FindLaneEntryRings(Rings, &Index, &Direction, &Position, ...)`
/// (`:483-532`): from ring `index` along the direction, the first ring with
/// a ring behind it (`index - direction`) farther than 5000 from the
/// position (the refined estimate of the length); `None` when the walk
/// leaves the lane.
fn find_lane_entry_rings(
    rings: &[systems::TradeLaneRing],
    mut index: i32,
    direction: i32,
    position: Vector,
) -> Option<(i32, i32)> {
    let count = rings.len() as u32;
    loop {
        if index as u32 >= count {
            return None;
        }
        let ring = rings[index as usize].position;
        let check = (direction == 1 && index != 0)
            || (direction == -1 && (index as u32) < count.wrapping_sub(1));
        if check {
            let dx = f64::from(ring.x) - f64::from(position.x);
            let dy = f64::from(ring.y) - f64::from(position.y);
            let dz = f64::from(ring.z) - f64::from(position.z);
            let length_sq = (dz * dz + dy * dy) + dx * dx;
            let y = rsqrt_guess(length_sq as f32);
            let length = ((1.5 - y * y * length_sq * 0.5) * y) * length_sq;
            if LANE_ENTRY_DISTANCE < length {
                return Some((index, index - direction));
            }
        }
        index += direction;
    }
}

/// `PickLaneEntry(System, Rings, Request, &bInLaneZone, ...)` (`:535-637`):
/// the entry ring and the ring before it from the ring nearest to the
/// request (the scan stops once the distances grow) towards the lane's far
/// end, or a random way from a middle ring. The answer then depends on the
/// lane end the ships come from: with a player's ship within 4000 of it,
/// seeing it (4000, 70°) or in a lane whose end ring is within 4000 of it,
/// it is the opposite of "middle" (a request not at the ship, or a ship in
/// a lane zone whose nearest ring is no end ring), else "middle". `None`
/// when no entry ring is found.
fn pick_lane_entry(
    cx: &mut Cx<'_>,
    system: u32,
    rings: &[systems::TradeLaneRing],
    request: &SpawnRequest,
    in_lane_zone: bool,
) -> Option<(bool, i32, i32)> {
    let position = request.position;
    let mut nearest = -1_i32;
    let mut nearest_sq = -1.0_f32;
    for (index, ring) in rings.iter().enumerate() {
        let dx = f64::from(position.x) - f64::from(ring.position.x);
        let dy = f64::from(position.y) - f64::from(ring.position.y);
        let dz = f64::from(position.z) - f64::from(ring.position.z);
        let d = (dz * dz + dy * dy) + dx * dx;
        if nearest_sq == -1.0 || f64::from(nearest_sq) > d {
            nearest = index as i32;
            nearest_sq = d as f32;
        } else if f64::from(nearest_sq) < d {
            break;
        }
    }
    let last = rings.len() as i32 - 1;
    let middle = if request.zone_list != 0 {
        true
    } else {
        !(!in_lane_zone || nearest == 0 || nearest == last)
    };
    let (ring, prev, end) = if nearest == 0 {
        let (r, p) = find_lane_entry_rings(rings, 0, 1, position)?;
        (r, p, rings.first()?.position)
    } else if nearest == last {
        let (r, p) = find_lane_entry_rings(rings, nearest, -1, position)?;
        (r, p, rings.last()?.position)
    } else {
        let (first, second) = if rand_coin(cx) == 1 { (1, -1) } else { (-1, 1) };
        let (r, p) = find_lane_entry_rings(rings, nearest, first, position)
            .or_else(|| find_lane_entry_rings(rings, nearest, second, position))?;
        let end = if p > r {
            rings.last()?.position
        } else {
            rings.first()?.position
        };
        (r, p, end)
    };
    let seen = systems::player_ship_near(cx, system, end, 4000.0).0
        || systems::is_seen_by_player_ship(cx, system, end, 4000.0, 70.0)
        || systems::is_near_player_lane_end(cx, system, end, 4000.0);
    Some((if seen { !middle } else { middle }, ring, prev))
}

/// `PickLane`: the only list, or `rand() * count / 32768`.
fn pick_lane(cx: &mut Cx<'_>, count: usize) -> usize {
    if count == 1 { 0 } else { rand_index(cx, count) }
}

/// The arrival point of a trade lane: the entry ring, the ring before it,
/// the point and the orientation.
struct LaneArrival {
    ring: u32,
    next_ring: u32,
    position: Vector,
    orientation: Matrix,
}

/// `PickTradeLaneArrival(System, Zone, Request, ...)` (`:651-803`): the ring
/// lists of the lanes within 5000 of the request (in a lane zone of the
/// heartbeat population at the ship: those whose first ring is in or on
/// the zone) tried in random order, each lane's entry rings from
/// [`pick_lane_entry`]. The exit point is `offset` beyond the entry ring
/// along its z axis (behind it when the lane runs the other way); it is
/// rejected when a ship travelling that way is within 15000 of it or, near
/// a lane end, one left less than 7 s ago. The point is 255 above the exit
/// point, the orientation the ring's (turned around for the reverse way).
fn pick_trade_lane_arrival(
    cx: &mut Cx<'_>,
    system: u32,
    zone: Option<usize>,
    request: &SpawnRequest,
    offset: f32,
) -> Option<LaneArrival> {
    let record = cx.logic.content.state.systems.get(&system)?;
    let zone_info = zone
        .and_then(|z| record.zones.get(z))
        .map(|z| (z.zone_id, z.tradelane));
    let mut lanes: Vec<Vec<systems::TradeLaneRing>> =
        systems::find_trade_lane_ring_lists_near(record, request.position, 5000.0)
            .into_iter()
            .map(|(_, r)| r)
            .collect();
    let in_lane_zone = matches!(zone_info, Some((_, true))) && request.zone_list == 0;
    if in_lane_zone {
        let zone_id = zone_info.map_or(0, |z| z.0);
        let mut kept = Vec::new();
        for lane in lanes {
            let first = lane.first().map_or(Vector::ZERO, |r| r.position);
            if !(api::zone::classify_point(cx, zone_id, first) > 1.0) {
                kept.push(lane);
            }
        }
        lanes = kept;
    }
    if lanes.is_empty() {
        return None;
    }
    loop {
        if lanes.is_empty() {
            return None;
        }
        let mut at = pick_lane(cx, lanes.len());
        let (ring_index, prev_index) = loop {
            if let Some((true, r, p)) =
                pick_lane_entry(cx, system, &lanes[at], request, in_lane_zone)
            {
                break (r, p);
            }
            lanes.remove(at);
            if lanes.is_empty() {
                return None;
            }
            at = pick_lane(cx, lanes.len());
        };
        let rings = lanes[at].clone();
        let ring = rings[ring_index as usize].object;
        let next_ring = rings[prev_index as usize].object;
        let (ring_position, ring_orientation) =
            api::space_obj::location(cx, ring).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
        let (prev_position, _) =
            api::space_obj::location(cx, next_ring).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
        let lane_direction = sub(prev_position, ring_position);
        let near_lane_end = if prev_index < ring_index {
            ring_index == 1 || ring_index == 2
        } else if prev_index > ring_index {
            let count = rings.len() as i32;
            ring_index == count - 2 || ring_index == count - 3
        } else {
            false
        };
        let m = &ring_orientation.m;
        let dot = (f64::from(lane_direction.x) * f64::from(m[0][2])
            + f64::from(lane_direction.y) * f64::from(m[1][2]))
            + f64::from(lane_direction.z) * f64::from(m[2][2]);
        let forward = dot < 0.0;
        let direction = if forward { 1 } else { -1 };
        let local = Vector::new(0.0, 0.0, if forward { -offset } else { offset });
        let exit_point = add(transform_matrix(&ring_orientation, local), ring_position);
        let record = cx.logic.content.state.systems.get(&system)?;
        let leave_time = systems::time_since_lane_leave(cx, record, direction, ring) as f32;
        let ship_distance = systems::lane_ship_distance(cx, record, direction, ring, exit_point);
        let reject = match ship_distance {
            Some(d) if !(f64::from(d).abs() > 15000.0) => true,
            _ => near_lane_end && !(leave_time >= 7.0) && !(leave_time < 0.0),
        };
        if reject {
            lanes.remove(at);
            continue;
        }
        let mut orientation = ring_orientation;
        let lift = if forward {
            Vector::new(0.0, 255.0, -offset)
        } else {
            for row in 0..3 {
                orientation.m[row][0] = -ring_orientation.m[row][0];
                orientation.m[row][2] = -ring_orientation.m[row][2];
            }
            Vector::new(0.0, -255.0, offset)
        };
        let position = add(transform_matrix(&ring_orientation, lift), ring_position);
        return Some(LaneArrival {
            ring,
            next_ring,
            position,
            orientation,
        });
    }
}

/// `BuildColumnMatrix(Right, Forward, &UpRaw)` (`:806-829`): the columns
/// right, up and forward, with `Up = Forward x Right` (each product stored)
/// normalised; the raw up too.
fn build_column_matrix(right: Vector, forward: Vector) -> (Matrix, Vector) {
    let up = Vector::new(
        (f64::from(right.z) * f64::from(forward.y) - f64::from(right.y) * f64::from(forward.z))
            as f32,
        (f64::from(forward.z) * f64::from(right.x) - f64::from(right.z) * f64::from(forward.x))
            as f32,
        (f64::from(right.y) * f64::from(forward.x) - f64::from(forward.y) * f64::from(right.x))
            as f32,
    );
    let inv_up = 1.0 / length(up);
    let m = Matrix::from_rows([
        [right.x, (f64::from(up.x) * inv_up) as f32, forward.x],
        [right.y, (f64::from(up.y) * inv_up) as f32, forward.y],
        [right.z, (f64::from(up.z) * inv_up) as f32, forward.z],
    ]);
    (m, up)
}

/// `(0, 1, 0) x forward` unrounded, as the cruise and the buzz spell it.
fn right_of(forward: Vector) -> (f64, f64, f64) {
    let y0 = f64::from(forward.y) * 0.0;
    (
        f64::from(forward.z) - y0,
        f64::from(forward.x) * 0.0 - f64::from(forward.z) * 0.0,
        y0 - f64::from(forward.x),
    )
}

/// The leader's guide description; the C++ reads it without a test.
fn guide_kind(blocks: &[NpcShipParams]) -> i32 {
    blocks
        .first()
        .and_then(|b| b.guide.as_ref())
        .map_or(0, |g| g.kind)
}

/// `PlaceOnTradeLane(...)` (`:845-890`), arrival method 1:
/// [`pick_trade_lane_arrival`] with the offset 0 for the zones ahead (zone
/// list 2), else 3750; the entry ring must be outside every sphere of the
/// request. Plan kind 1 with both rings, the leader at the arrival point and
/// orientation, and an `InstantTradelane` directive (`0x14`) in front of
/// its list.
fn place_on_trade_lane(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    system: u32,
    zone: Option<usize>,
    plan: &mut SpawnPlan,
    blocks: &mut [NpcShipParams],
) -> bool {
    let offset = if request.zone_list == 2 { 0.0 } else { 3750.0 };
    let Some(arrival) = pick_trade_lane_arrival(cx, system, zone, request, offset) else {
        return false;
    };
    let (ring_position, _) =
        api::space_obj::location(cx, arrival.ring).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    if !test_exclusion_spheres(ring_position, Some(&request.spheres), true, false) {
        return false;
    }
    plan.ring = arrival.ring;
    plan.next_ring = arrival.next_ring;
    plan.kind = 1;
    let Some(leader) = blocks.first_mut() else {
        return true;
    };
    leader.position = arrival.position;
    leader.orientation = arrival.orientation;
    let mut lane = InstantTradelane::default();
    lane.op.entrance = arrival.ring;
    lane.op.speed = offset;
    lane.op.exit = arrival.next_ring;
    add_leader_directive(cx, leader, DirectiveKind::InstantTradelane(lane));
    true
}

/// `PlaceAtDock(...)` (`:893-1007`), arrival methods 2 to 6, only for a
/// formation of one ship: the object types from the methods 3 to 6
/// (docking ring `0x20`, jump gate `0x40`, station `0x100`, capital
/// `0x200000`; an arrival drops rings and gates). Ships that can use
/// moorings look within 4000 and must be at least 3500 away (an arrival:
/// at most); other ships look within 2700 and must be within it (ships
/// without berths only from rings, gates and stations with bits `0xc40`;
/// arrivals without moorings never launch). Three picks, each excluding the
/// last; each launcher must be outside the request's spheres and 2500 from
/// the other players' ships. Plan kind 2 with the launcher.
#[allow(clippy::too_many_arguments)]
fn place_at_dock(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    system: u32,
    methods: &[u8; 9],
    plan: &mut SpawnPlan,
    faction: u32,
    blocks: &[NpcShipParams],
) -> bool {
    if blocks.len() != 1 {
        return false;
    }
    let mut mask = (if methods[5] != 0 { 0x100 } else { 0 })
        | (if methods[6] != 0 { 0x20_0000 } else { 0 })
        | (if methods[4] != 0 { 0x40 } else { 0 })
        | (if methods[3] != 0 { 0x20 } else { 0 });
    let classes = blocks[0].class_ids.clone();
    if request.arrival {
        mask &= 0xffff_f39f;
    }
    let id = neuron_core::id::create_id;
    let has = |name: &[u8]| classes.contains(&id(name));
    let mut try_it = true;
    let mut near = false;
    let mut range = 0.0;
    if has(b"can_use_small_moors") || has(b"can_use_medium_moors") || has(b"can_use_large_moors") {
        range = 4000.0;
    } else if has(b"can_use_berths") {
        if request.arrival {
            try_it = false;
        } else {
            range = super::encounters::PLACEMENT_RANGE;
            near = true;
        }
    } else if request.arrival || mask & 0xc40 == 0 {
        try_it = false;
    } else {
        range = super::encounters::PLACEMENT_RANGE;
        near = true;
    }
    plan.dock_target = 0;
    let mut spheres: Vec<[f32; 4]> = Vec::new();
    for _ in 0..3 {
        if !try_it {
            break;
        }
        let query = systems::DockTargetQuery {
            system,
            classes: &classes,
            group: faction,
            exclude: plan.dock_target,
            position: request.position,
            range,
            type_mask: mask,
        };
        plan.dock_target = systems::pick_dock_target(cx, &query);
        if plan.dock_target & 0x3fff_ffff == 0 {
            continue;
        }
        let (position, _) = api::space_obj::location(cx, plan.dock_target)
            .unwrap_or((Vector::ZERO, Matrix::IDENTITY));
        if spheres.is_empty() {
            spheres.extend_from_slice(&request.spheres);
            add_other_player_spheres(cx, system, request, &mut spheres);
        }
        if !test_exclusion_spheres(position, Some(&spheres), true, false) {
            continue;
        }
        let from = request.position;
        let dx = f64::from(position.x) - f64::from(from.x);
        let dy = f64::from(position.y) - f64::from(from.y);
        let dz = f64::from(position.z) - f64::from(from.z);
        let distance_sq = (dz * dz + dy * dy) + dx * dx;
        let range_sq = f64::from(super::encounters::PLACEMENT_RANGE)
            * f64::from(super::encounters::PLACEMENT_RANGE);
        if near {
            if range_sq < distance_sq {
                continue;
            }
        } else if !request.arrival {
            if distance_sq < DOCK_LAUNCH_DISTANCE_SQ {
                continue;
            }
        } else if distance_sq > DOCK_LAUNCH_DISTANCE_SQ {
            continue;
        }
        plan.kind = 2;
        return true;
    }
    false
}

/// The leader's `GotoVec` to the destination (factory 5): priority 1, the
/// range, `Field_58` 1.
fn leader_goto(cx: &mut Cx<'_>, leader: &mut NpcShipParams, destination: Vector, range: f32) {
    let mut goto = Goto {
        priority: 1,
        ..Goto::default()
    };
    npc_ships::set_goto_destination(&mut goto, destination, range, -1.0, 0, false, false);
    goto.op.field_58 = 1;
    add_leader_directive(cx, leader, DirectiveKind::GotoVec(goto));
}

/// `PlaceCruising(...)` (`:1010-1135`), arrival method 7. The spheres:
/// 2500 around the other players' ships, the request's, and one around the
/// request's position. A patrol leader (guide type 3) spawns on a random
/// segment of the zone's path ([`find_segment_spawn_point`], the sphere 200
/// for arrivals, 2500 else; the plan's path, target point and direction);
/// any other formation on a sphere around the request (an arrival: radius
/// `100 + rand * 1775`, sphere 100; else around the position in 5 s,
/// radius `2500 + rand * 50`, looking from the position in 7 s, sphere
/// 2500) through [`find_sphere_spawn_point`] (cylinder 2000 by 500). Plan
/// kind 7; every block faces the destination, and the leader gets a
/// `GotoVec` (500 on a path, else 100).
///
/// A patrol zone without a path or segments, which the C++ reads through
/// null or past the end, places nothing (the segment's draw is made;
/// D1063).
#[allow(clippy::too_many_arguments)]
fn place_cruising(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    system: u32,
    zone: Option<usize>,
    zone_creation_distance: f64,
    plan: &mut SpawnPlan,
    blocks: &mut [NpcShipParams],
) -> bool {
    let mut spheres: Vec<[f32; 4]> = Vec::new();
    let zone_info = zone.and_then(|z| {
        cx.logic
            .content
            .state
            .systems
            .get(&system)
            .and_then(|r| r.zones.get(z))
            .map(|zz| (zz.zone_id, zz.path))
    });
    let zones: Vec<u32> = zone_info.iter().map(|z| z.0).collect();
    add_other_player_spheres(cx, system, request, &mut spheres);
    spheres.extend_from_slice(&request.spheres);
    let at = request.position;
    let max_distance = zone_creation_distance as f32;
    let found: Option<(Vector, Vector)>;
    if guide_kind(blocks) == 3 {
        spheres.push([
            at.x,
            at.y,
            at.z,
            if request.arrival { 200.0 } else { 2500.0 },
        ]);
        let path_index = zone_info.and_then(|z| z.1);
        let path = path_index.and_then(|p| {
            cx.logic
                .content
                .state
                .systems
                .get(&system)
                .and_then(|r| r.paths.get(p))
                .cloned()
        });
        let segments = path
            .as_ref()
            .and_then(|p| p.zone_segments(zone_info.map_or(0, |z| z.0)))
            .unwrap_or_default();
        let pick = rand_index(cx, segments.len());
        let (Some(path), Some(segment)) = (path, segments.get(pick).copied()) else {
            return false;
        };
        let start = path.points.get(segment.first_point as usize).copied();
        let end = path.points.get(segment.second_point as usize).copied();
        let (Some(start), Some(end)) = (start, end) else {
            return false;
        };
        let point =
            find_segment_spawn_point(cx, &spheres, &zones, max_distance, at, start, end, system);
        found = point.map(|p| {
            plan.path = path_index.map(|index| PatrolPathRef { system, index });
            plan.path_direction = p.direction;
            plan.path_target_point = if p.direction == 1 {
                segment.second_point
            } else {
                segment.first_point
            };
            (p.position, p.target)
        });
    } else {
        let (center, look_from, radius);
        if request.arrival {
            radius = (f64::from(cx.world.rng().rand()) * RAND_SCALE * 1775.0 + 100.0) as f32;
            center = at;
            look_from = at;
            spheres.push([at.x, at.y, at.z, 100.0]);
        } else {
            radius = (f64::from(cx.world.rng().rand()) * RAND_SCALE * 50.0 + 2500.0) as f32;
            center = add(at, scale(request.velocity, 5.0));
            let ahead = Vector::new(
                request.velocity.x * 7.0,
                request.velocity.y * 7.0,
                request.velocity.z * 7.0,
            );
            look_from = add(at, ahead);
            spheres.push([at.x, at.y, at.z, 2500.0]);
        }
        found = find_sphere_spawn_point(
            cx,
            &spheres,
            &zones,
            max_distance,
            center,
            radius,
            look_from,
            2000.0,
            500.0,
            system,
        );
    }
    let Some((spawn, destination)) = found else {
        return false;
    };
    plan.position = spawn;
    plan.kind = 7;
    plan.destination = destination;
    if let Some(leader) = blocks.first_mut() {
        leader.position = spawn;
    }
    let delta = sub(spawn, destination);
    let inv_length = 1.0
        / ((f64::from(delta.x) * f64::from(delta.x) + f64::from(delta.z) * f64::from(delta.z))
            + f64::from(delta.y) * f64::from(delta.y))
        .sqrt();
    let forward = scaled(delta, inv_length);
    let (rx, ry, rz) = right_of(forward);
    let inv_right = (1.0 / ((rx * rx + ry * ry) + rz * rz).sqrt()) as f32;
    let right = Vector::new(
        (rx * f64::from(inv_right)) as f32,
        (ry * f64::from(inv_right)) as f32,
        (rz * f64::from(inv_right)) as f32,
    );
    let (orientation, _) = build_column_matrix(right, forward);
    for block in blocks.iter_mut() {
        block.orientation = orientation;
    }
    let range = if guide_kind(blocks) == 3 {
        500.0
    } else {
        100.0
    };
    if let Some(leader) = blocks.first_mut() {
        leader_goto(cx, leader, destination, range);
    }
    true
}

/// `PlaceBuzzing(...)` (`:1146-1306`), arrival method 8. The spheres: the
/// request's, 2500 around the other players' ships, and (not for arrivals)
/// 2500 around the request's position. A ship slower than 5 gets a point
/// on a sphere of radius `2500 + rand * 50` around it; a moving one a point
/// on a sphere of 45 placed 150 above its path, `2500 + speed * 2.5` ahead,
/// looking from 5000 behind it (straight up or down: the slow case around
/// the ship, looking from behind). [`find_sphere_spawn_point`] with a
/// cylinder of 50 by 50. Plan kind 8; every block faces the destination;
/// the leader is lifted 50 along the up axis and gets a `GotoVec` (401).
#[allow(clippy::too_many_arguments)]
fn place_buzzing(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    system: u32,
    zone: Option<usize>,
    zone_creation_distance: f64,
    plan: &mut SpawnPlan,
    blocks: &mut [NpcShipParams],
) -> bool {
    let velocity = request.velocity;
    let zones: Vec<u32> = zone
        .and_then(|z| {
            cx.logic
                .content
                .state
                .systems
                .get(&system)
                .and_then(|r| r.zones.get(z))
                .map(|zz| zz.zone_id)
        })
        .into_iter()
        .collect();
    let mut spheres: Vec<[f32; 4]> = request.spheres.clone();
    add_other_player_spheres(cx, system, request, &mut spheres);
    let at = request.position;
    let speed_sq = ((f64::from(velocity.x) * f64::from(velocity.x)
        + f64::from(velocity.y) * f64::from(velocity.y))
        + f64::from(velocity.z) * f64::from(velocity.z)) as f32;
    let speed = fast_reciprocal_sqrt(speed_sq) * f64::from(speed_sq);
    let mut center;
    let look_from;
    let radius;
    if speed <= 5.0 {
        center = at;
        look_from = at;
        radius = (f64::from(cx.world.rng().rand()) * RAND_SCALE * 50.0 + 2500.0) as f32;
    } else {
        let direction = scaled(velocity, 1.0 / length(velocity));
        let ahead_distance =
            (fast_reciprocal_sqrt(speed_sq) * f64::from(speed_sq) * 2.5 + 2500.0) as f32;
        let ahead = add(at, scale(direction, ahead_distance));
        center = ahead;
        let behind = sub(at, scale(direction, 5000.0));
        look_from = behind;
        let track = sub(ahead, behind);
        let track_direction = scaled(track, 1.0 / length(track));
        let (rx, ry, rz) = right_of(track_direction);
        let mut right = Vector::new(rx as f32, ry as f32, rz as f32);
        if right.x == 0.0 && right.y == 0.0 && right.z == 0.0 {
            center = at;
            radius = (f64::from(cx.world.rng().rand()) * RAND_SCALE * 50.0 + 2500.0) as f32;
        } else {
            right = scaled(right, 1.0 / length(right));
            let t = track_direction;
            let up = Vector::new(
                (f64::from(t.y) * f64::from(right.z) - f64::from(t.z) * f64::from(right.y)) as f32,
                (f64::from(t.z) * f64::from(right.x) - f64::from(t.x) * f64::from(right.z)) as f32,
                (f64::from(t.x) * f64::from(right.y) - f64::from(t.y) * f64::from(right.x)) as f32,
            );
            let up = scaled(up, 1.0 / length(up));
            let raise = scale(up, 150.0);
            center = add(center, raise);
            radius = 45.0;
        }
    }
    if !request.arrival {
        spheres.push([at.x, at.y, at.z, 2500.0]);
    }
    let Some((spawn, destination)) = find_sphere_spawn_point(
        cx,
        &spheres,
        &zones,
        zone_creation_distance as f32,
        center,
        radius,
        look_from,
        50.0,
        50.0,
        system,
    ) else {
        return false;
    };
    plan.position = spawn;
    plan.kind = 8;
    plan.destination = destination;
    let delta = sub(spawn, destination);
    let forward = scaled(delta, 1.0 / length(delta));
    let (rx, ry, rz) = right_of(forward);
    let right = Vector::new(rx as f32, ry as f32, rz as f32);
    let right = scaled(right, 1.0 / length(right));
    let (orientation, up_raw) = build_column_matrix(right, forward);
    for block in blocks.iter_mut() {
        block.orientation = orientation;
    }
    let up = scaled(up_raw, 1.0 / length(up_raw));
    let lift = scale(up, 50.0);
    if let Some(leader) = blocks.first_mut() {
        leader.position = add(spawn, lift);
        leader_goto(cx, leader, destination, 401.0);
    }
    true
}

/// `PlaceSpawnFormation(Request, System, Zone, &ZoneCreationDistance,
/// Arrival, Plan, &Faction, Blocks)` (`:1309-1376`): arrivals may cruise in
/// and, unless the leader is a patrol leader, buzz the player but never
/// come out of a lane. Then, until the plan has a kind, a method drawn at
/// random (`rand() * 9 / 32768`, then the next set one, wrapping) is
/// tried and dropped (the dock methods 2 to 6 together). False when no
/// method is left.
#[allow(clippy::too_many_arguments)]
pub fn place_spawn_formation(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    system: u32,
    zone: Option<usize>,
    zone_creation_distance: f64,
    arrival: [u8; 9],
    plan: &mut SpawnPlan,
    faction: u32,
    blocks: &mut [NpcShipParams],
) -> bool {
    let mut methods = arrival;
    if request.arrival {
        methods[7] = 1;
        let leader_patrols = blocks
            .first()
            .and_then(|b| b.guide.as_ref())
            .is_some_and(|g| g.kind == 3);
        if !leader_patrols {
            methods[8] = 1;
            methods[1] = 0;
        }
    }
    while plan.kind == 0 {
        if methods.iter().all(|&m| m == 0) {
            return false;
        }
        let count = methods.len() as u32;
        let mut pick = (cx.world.rng().rand().wrapping_mul(count as i32) / 32768) as u32;
        while methods[(pick % count) as usize] == 0 {
            pick += 1;
        }
        match pick % count {
            1 => {
                place_on_trade_lane(cx, request, system, zone, plan, blocks);
                methods[1] = 0;
            }
            2..=6 => {
                place_at_dock(cx, request, system, &methods, plan, faction, blocks);
                for m in &mut methods[2..=6] {
                    *m = 0;
                }
            }
            7 => {
                place_cruising(
                    cx,
                    request,
                    system,
                    zone,
                    zone_creation_distance,
                    plan,
                    blocks,
                );
                methods[7] = 0;
            }
            8 => {
                place_buzzing(
                    cx,
                    request,
                    system,
                    zone,
                    zone_creation_distance,
                    plan,
                    blocks,
                );
                methods[8] = 0;
            }
            // Method 0 is never set by the arrival parser.
            _ => methods[0] = 0,
        }
    }
    plan.kind != 0
}

/// `PickLeaderDockTarget`: the leader's classes, no ship reputation, the
/// leader's affiliation (fetched for each pick) and the object types
/// `0x600d60`.
fn pick_leader_dock_target(
    cx: &mut Cx<'_>,
    system: u32,
    leader: &NpcShipParams,
    exclude: u32,
    position: Vector,
    range: f32,
) -> u32 {
    let affiliation = super::rep_handles::affiliation(cx, leader.reputation);
    let query = systems::DockTargetQuery {
        system,
        classes: &leader.class_ids,
        group: affiliation,
        exclude,
        position,
        range,
        type_mask: 0x60_0d60,
    };
    systems::pick_dock_target(cx, &query)
}

/// `KeepPointsInRange`: the point indices within `[first, last]`.
fn keep_points_in_range(points: &mut Vec<i32>, first: i32, last: i32) {
    points.retain(|&p| !(p < first || p > last));
}

/// The path of a plan, cloned from its record.
fn plan_path(cx: &Cx<'_>, plan: &SpawnPlan) -> Option<super::patrol_paths::PatrolPath> {
    let path = plan.path?;
    cx.logic
        .content
        .state
        .systems
        .get(&path.system)?
        .paths
        .get(path.index)
        .cloned()
}

/// A random point of `points` (`rand() * n / 32768`); the C++ reads an
/// empty list past its end, here -1 (D1063).
fn random_point(cx: &mut Cx<'_>, points: &[i32]) -> i32 {
    let at = rand_index(cx, points.len());
    points.get(at).copied().unwrap_or(-1)
}

/// `SetLeaderGuideTargets(System, Plan, Leader)` (`:1406-1627`): the two
/// targets of the leader's guide.
///
/// - **Launched from a dock** (kind 2): the launcher first; the second is
///   the launcher for a wander guide, on a path a dock target of the path
///   (for a trade path from the target point on, or up to it), else a dock
///   target anywhere; the plan's path point is a random point of the
///   second.
/// - **Out of a lane** (kind 1): from the lane's end the ships came out at,
///   a dock target within 20000 of the lane's other end. A wander guide's
///   first target is that one (else anywhere). Otherwise, half the time or
///   when one lane is near, a target within 20000 of the end; else the far
///   end of a random other lane within 20000, until one is found (then
///   anywhere); the second, when none, anywhere.
/// - **Cruising or buzzing** (or not placed): on a path, a target of the
///   path's range by its usage and direction, then a second at least 5000
///   from it in the other range (looped paths: between the target point
///   and the path point); off a path, half the time a target within 20000
///   of the leader, else anywhere, and the second anywhere. A wander guide
///   takes one target for both when one is missing.
///
/// The description's `SetTargets` gets the second target first, but for a
/// dock launch.
pub fn set_leader_guide_targets(
    cx: &mut Cx<'_>,
    system: u32,
    plan: &mut SpawnPlan,
    leader: &mut NpcShipParams,
) {
    let Some(mut desc) = leader.guide.take() else {
        return;
    };
    set_targets_of(cx, system, plan, leader, &mut desc);
    leader.guide = Some(desc);
}

/// The body of [`set_leader_guide_targets`] on the leader's description.
fn set_targets_of(
    cx: &mut Cx<'_>,
    system: u32,
    plan: &mut SpawnPlan,
    leader: &NpcShipParams,
    desc: &mut GuideDesc,
) {
    let wander = desc.kind == 4;
    let origin = Vector::ZERO;
    let vibe = cx.logic.content.state.rep_handles.vibe(leader.reputation);
    if plan.kind == 2 {
        let dock_target = plan.dock_target;
        let mut target2;
        if wander {
            target2 = dock_target;
        } else if let Some(path) = plan_path(cx, plan) {
            let (mut first, mut last) = (-1, -1);
            if path.usage & 0x10 != 0 {
                if plan.path_direction == 1 {
                    first = plan.path_target_point;
                } else {
                    first = 0;
                    last = plan.path_target_point;
                }
            }
            let affiliation = super::rep_handles::affiliation(cx, leader.reputation);
            target2 = systems::pick_path_dock_target(
                cx,
                &path,
                &leader.class_ids,
                affiliation,
                dock_target,
                first,
                last,
                0.0,
                u32::MAX,
            );
            let mut points = path.object_point_indices(target2);
            keep_points_in_range(&mut points, first, last);
            plan.path_point = random_point(cx, &points);
            if target2 == 0 {
                target2 = dock_target;
            }
        } else {
            target2 = pick_leader_dock_target(cx, system, leader, dock_target, origin, -1.0);
        }
        desc.set_targets(cx, dock_target, target2, vibe);
        return;
    }
    if plan.kind == 1 {
        let Some(record) = cx.logic.content.state.systems.get(&system) else {
            return;
        };
        let order = systems::trade_lane_ring_order(record, plan.ring, plan.next_ring);
        let lane_index = record.lane_of_ring(plan.ring);
        let lane = lane_index.map(|i| {
            let l = &record.trade_lanes[i];
            (l.start, l.end)
        });
        let (start, end) = lane.unwrap_or((Vector::ZERO, Vector::ZERO));
        let mut target2;
        let mut target1 = 0;
        let anchor = match order {
            -1 => {
                target2 = pick_leader_dock_target(cx, system, leader, 0, end, 20000.0);
                start
            }
            0 => return,
            _ => {
                target2 = pick_leader_dock_target(cx, system, leader, 0, start, 20000.0);
                end
            }
        };
        if wander {
            if target2 == 0 {
                target2 = pick_leader_dock_target(cx, system, leader, 0, origin, -1.0);
            }
            target1 = target2;
        } else {
            let mut near = cx
                .logic
                .content
                .state
                .systems
                .get(&system)
                .map(|r| systems::find_trade_lanes_near(r, anchor, 20000.0))
                .unwrap_or_default();
            let one_lane = near.len() == 1;
            if f64::from(cx.world.rng().rand()) * RAND_SCALE < 0.5 || one_lane {
                target1 = pick_leader_dock_target(cx, system, leader, target2, anchor, 20000.0);
            }
            if let Some(own) = lane_index
                && let Some(at) = near.iter().position(|&l| l == own)
            {
                near.remove(at);
            }
            while target1 == 0 {
                if near.is_empty() {
                    target1 = pick_leader_dock_target(cx, system, leader, target2, origin, -1.0);
                    break;
                }
                let pick = rand_index(cx, near.len());
                let (other_start, other_end) = cx
                    .logic
                    .content
                    .state
                    .systems
                    .get(&system)
                    .and_then(|r| r.trade_lanes.get(near[pick]))
                    .map_or((Vector::ZERO, Vector::ZERO), |l| (l.start, l.end));
                let to_start = sub(other_start, anchor);
                let to_end = sub(other_end, anchor);
                let sq = |v: Vector| {
                    (f64::from(v.z) * f64::from(v.z) + f64::from(v.y) * f64::from(v.y))
                        + f64::from(v.x) * f64::from(v.x)
                };
                let far_end = if sq(to_end) > sq(to_start) {
                    other_end
                } else {
                    other_start
                };
                target1 = pick_leader_dock_target(cx, system, leader, target2, far_end, 20000.0);
                near.remove(pick);
            }
            if target2 == 0 {
                target2 = pick_leader_dock_target(cx, system, leader, target1, origin, -1.0);
            }
        }
        desc.set_targets(cx, target2, target1, vibe);
        return;
    }
    let mut target1;
    let mut target2;
    if let Some(path) = plan_path(cx, plan) {
        let last_point = path.points.len() as i32 - 1;
        let (first, last) = if path.usage & 0x10 != 0 {
            if plan.path_direction == 1 {
                (plan.path_target_point, last_point)
            } else {
                (0, plan.path_target_point)
            }
        } else {
            (0, last_point)
        };
        let affiliation = super::rep_handles::affiliation(cx, leader.reputation);
        target1 = systems::pick_path_dock_target(
            cx,
            &path,
            &leader.class_ids,
            affiliation,
            0,
            first,
            last,
            0.0,
            u32::MAX,
        );
        let mut points = path.object_point_indices(target1);
        keep_points_in_range(&mut points, first, last);
        plan.path_point = random_point(cx, &points);
        let (second_first, second_last) = if path.looped {
            (plan.path_target_point, plan.path_point)
        } else if path.usage & 0x10 != 0 {
            if plan.path_direction == 1 {
                (0, plan.path_target_point)
            } else {
                (plan.path_target_point, last_point)
            }
        } else {
            (0, last_point)
        };
        let affiliation = super::rep_handles::affiliation(cx, leader.reputation);
        target2 = systems::pick_path_dock_target(
            cx,
            &path,
            &leader.class_ids,
            affiliation,
            target1,
            second_first,
            second_last,
            5000.0,
            u32::MAX,
        );
        if target2 == 0 {
            target2 = target1;
        }
    } else {
        target1 = 0;
        if f64::from(cx.world.rng().rand()) * RAND_SCALE < 0.5 {
            target1 = pick_leader_dock_target(cx, system, leader, 0, leader.position, 20000.0);
        }
        if target1 == 0 {
            target1 = pick_leader_dock_target(cx, system, leader, 0, origin, -1.0);
        }
        target2 = pick_leader_dock_target(cx, system, leader, target1, origin, -1.0);
    }
    if wander {
        if target2 == 0 {
            target2 = target1;
        } else if target1 == 0 {
            target1 = target2;
        }
    }
    desc.set_targets(cx, target2, target1, vibe);
}
