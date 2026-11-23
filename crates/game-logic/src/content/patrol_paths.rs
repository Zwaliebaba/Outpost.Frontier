//! The patrol paths of a system (`FPatrolPath`, `Content/PatrolPaths.cpp`,
//! the head of TU45): the zones that share one `path_label`, chained in
//! path-index order into a polyline (work package P3.4c, decision record
//! 0008 point 10).
//!
//! [`build_patrol_paths`] is `FSystemInfo::BuildPatrolPaths`
//! (`SystemInfo.cpp:1114-1209`), which the system record's loader calls
//! after the zones ([`super::zones::enumerate_system_zones`]); that loader
//! is P3.4d's. The guides that fly the paths are P3.6c's.

use std::collections::BTreeMap;

use game_sim::data::vc6::vc6_sort;
use neuron_core::math::{Transform, Vector, transform_xform};

use super::zones::{SystemZone, ZonePathLabel};
use crate::api;
use crate::server::Cx;

/// A space object near the path (`FPatrolPathObject`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PatrolPathObject {
    /// The object.
    pub object_id: u32,
    /// The indices of the path points it is near.
    pub point_indices: Vec<i32>,
}

/// One path step's two points (`FPatrolPathSegment`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PatrolPathSegment {
    /// The step index.
    pub first_point: i32,
    /// The step index plus 1.
    pub second_point: i32,
}

/// `FPatrolPath` (0x44 bytes).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PatrolPath {
    /// [`PatrolPath::register_object`]'s objects.
    pub objects: Vec<PatrolPathObject>,
    /// The shared `path_label`.
    pub label: u32,
    /// The polyline.
    pub points: Vec<Vector>,
    /// The steps of each zone (indices of the path's zones in path-index
    /// order); step `i` spans points `i` and `i + 1`.
    pub zone_steps: BTreeMap<u32, Vec<i32>>,
    /// The last point meets the first.
    pub looped: bool,
    /// The first label's `mission_eligible`.
    pub mission_eligible: bool,
    /// The first label's `usage` bits.
    pub usage: u32,
}

/// `SubtractVectors`, each component a float store.
fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) - f64::from(b.x)) as f32,
        (f64::from(a.y) - f64::from(b.y)) as f32,
        (f64::from(a.z) - f64::from(b.z)) as f32,
    )
}

/// `AddVectors`.
fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) + f64::from(b.x)) as f32,
        (f64::from(a.y) + f64::from(b.y)) as f32,
        (f64::from(a.z) + f64::from(b.z)) as f32,
    )
}

/// `DivideVector`: z scaled by the unrounded reciprocal, y and x by the
/// stored float.
fn divide(v: Vector, divisor: f32) -> Vector {
    let exact = 1.0 / f64::from(divisor);
    let stored = f64::from(exact as f32);
    Vector::new(
        (stored * f64::from(v.x)) as f32,
        (stored * f64::from(v.y)) as f32,
        (exact * f64::from(v.z)) as f32,
    )
}

/// The squared length summed x, z, y.
fn length_squared_xzy(v: Vector) -> f64 {
    let (x, y, z) = (f64::from(v.x), f64::from(v.y), f64::from(v.z));
    x * x + z * z + y * y
}

/// `DistanceSquaredToSegment` (TU53, `SystemInfo.cpp:1894-1951`): the
/// squared distance from `point` to the segment, and the segment's nearest
/// point. The x87 value is returned as a double.
#[must_use]
pub fn distance_squared_to_segment(point: Vector, start: Vector, end: Vector) -> (f64, Vector) {
    let to_point = sub(point, start);
    let along = sub(end, start);
    let dot = f64::from(along.z) * f64::from(to_point.z)
        + f64::from(along.y) * f64::from(to_point.y)
        + f64::from(along.x) * f64::from(to_point.x);
    let length_sq = f64::from(along.z) * f64::from(along.z)
        + f64::from(along.y) * f64::from(along.y)
        + f64::from(along.x) * f64::from(along.x);
    let fraction = dot / length_sq;
    let stored = fraction as f32;
    let (closest, interpolated) = if fraction > 1.0 {
        (end, false)
    } else if stored < 0.0 {
        (start, false)
    } else {
        let f = f64::from(stored);
        let scaled_z = (f64::from(along.z) * f) as f32;
        let closest = Vector::new(
            (f64::from(along.x) * f + f64::from(start.x)) as f32,
            (f64::from(along.y) * f + f64::from(start.y)) as f32,
            (f64::from(scaled_z) + f64::from(start.z)) as f32,
        );
        (closest, true)
    };
    let (ox, oy, oz) = if interpolated {
        (
            f64::from(point.x) - f64::from(closest.x),
            f64::from(point.y) - f64::from(closest.y),
            f64::from(point.z) - f64::from(closest.z),
        )
    } else {
        let o = sub(point, closest);
        (f64::from(o.x), f64::from(o.y), f64::from(o.z))
    };
    (oz * oz + oy * oy + ox * ox, closest)
}

/// `FSystemZone::GetPathEnds` (`Zones.cpp:31-73`): the zone's centre moved
/// by plus and minus half its y size along its own y axis
/// (`pub::Zone::GetSize`, `GetPos`, `GetOrientation`, `TransformXform`).
pub fn path_ends(cx: &mut Cx<'_>, zone: u32) -> (Vector, Vector) {
    let size = api::zone::size(cx, zone).unwrap_or(Vector::ZERO);
    let position = api::zone::pos(cx, zone);
    let orientation =
        api::zone::orientation(cx, zone).unwrap_or(neuron_core::math::Matrix::IDENTITY);
    let placement = Transform {
        rotation: orientation,
        translation: position,
    };
    let half = |sign: f64| (f64::from(size.y) * sign) as f32;
    let first = transform_xform(&placement, Vector::new(0.0, half(0.5), 0.0));
    let second = transform_xform(&placement, Vector::new(0.0, half(-0.5), 0.0));
    (first, second)
}

impl PatrolPath {
    /// `FPatrolPath(&First, &Last, System)` (`PatrolPaths.cpp:70-206`) over
    /// the labels `labels[first..last]` of one `path_label`: the labels in
    /// path-index order (one step per index, a label taken once per index
    /// it lists), each given its zone's two ends, then the polyline.
    ///
    /// For an index no label lists, the C++ takes the record at `last`,
    /// the first label of the next run, and writes that label's ends; the
    /// port does the same. Past the vector's end the C++ reads freed or
    /// foreign memory; the port leaves the step out (D796).
    pub fn new(cx: &mut Cx<'_>, labels: &mut [ZonePathLabel], first: usize, last: usize) -> Self {
        let mut path = Self {
            label: labels[first].label,
            usage: labels[first].usage,
            mission_eligible: labels[first].mission_eligible,
            ..Self::default()
        };
        let count: usize = labels[first..last].iter().map(|l| l.indices.len()).sum();
        let mut steps: Vec<usize> = Vec::new();
        for index in 1..=count as i32 {
            let step = (first..last)
                .find(|&s| labels[s].indices.contains(&index))
                .unwrap_or(last);
            if step >= labels.len() {
                continue;
            }
            let (start, end) = path_ends(cx, labels[step].zone_id);
            labels[step].start = start;
            labels[step].end = end;
            steps.push(step);
        }
        if steps.len() == 1 {
            let step = &labels[steps[0]];
            path.points.push(step.start);
            path.points.push(step.end);
        } else if steps.len() > 1 {
            let first_step = &labels[steps[0]];
            let second_step = &labels[steps[1]];
            let (start_distance, _) =
                distance_squared_to_segment(first_step.start, second_step.start, second_step.end);
            let start_distance = start_distance as f32;
            let (end_distance, _) =
                distance_squared_to_segment(first_step.end, second_step.start, second_step.end);
            let near = if f64::from(start_distance) < end_distance {
                first_step.start
            } else {
                first_step.end
            };
            let dx = f64::from(near.x) - f64::from(first_step.start.x);
            let dy = f64::from(near.y) - f64::from(first_step.start.y);
            let dz = f64::from(near.z) - f64::from(first_step.start.z);
            let mut current;
            if dx * dx <= 1.0 && dy * dy <= 1.0 && dz * dz <= 1.0 {
                path.points.push(first_step.end);
                current = first_step.start;
            } else {
                path.points.push(first_step.start);
                current = first_step.end;
            }
            for &s in &steps[1..] {
                let step = &labels[s];
                let to_start = sub(current, step.start);
                let to_end = sub(current, step.end);
                let joint;
                if length_squared_xzy(to_end) > length_squared_xzy(to_start) {
                    joint = divide(add(current, step.start), 2.0);
                    current = step.end;
                } else {
                    joint = divide(add(current, step.end), 2.0);
                    current = step.start;
                }
                path.points.push(joint);
            }
            let gap = sub(path.points[0], current);
            if length_squared_xzy(gap) < 25_000_000.0 {
                path.looped = true;
                let sum = add(current, path.points[0]);
                let middle = Vector::new(
                    (f64::from(sum.x) * 0.5) as f32,
                    (f64::from(sum.y) * 0.5) as f32,
                    (f64::from(sum.z) * 0.5) as f32,
                );
                path.points.push(middle);
                path.points[0] = middle;
            } else {
                path.looped = false;
                path.points.push(current);
            }
        }
        for (index, &s) in steps.iter().enumerate() {
            path.zone_steps
                .entry(labels[s].zone_id)
                .or_default()
                .push(index as i32);
        }
        path
    }

    /// `RegisterObject`: `object` recorded against every point closer than
    /// 5000 to `position`; true when one was.
    pub fn register_object(&mut self, object: u32, position: Vector) -> bool {
        let mut registered = false;
        for (index, point) in self.points.iter().enumerate() {
            let o = sub(*point, position);
            let (x, y, z) = (f64::from(o.x), f64::from(o.y), f64::from(o.z));
            let distance_sq = z * z + y * y + x * x;
            if distance_sq.partial_cmp(&25_000_000.0) != Some(std::cmp::Ordering::Less) {
                continue;
            }
            match self.objects.iter_mut().find(|e| e.object_id == object) {
                Some(entry) => entry.point_indices.push(index as i32),
                None => self.objects.push(PatrolPathObject {
                    object_id: object,
                    point_indices: vec![index as i32],
                }),
            }
            registered = true;
        }
        registered
    }

    /// `FindObjectsInRange`: copies of the objects (each once) whose type is
    /// in `type_mask` and that are near a point in `[first, last]` (-1 the
    /// path's first or last point); when `first > last` the range wraps.
    pub fn objects_in_range(
        &self,
        cx: &Cx<'_>,
        first: i32,
        last: i32,
        type_mask: u32,
    ) -> Vec<PatrolPathObject> {
        let first = if first == -1 { 0 } else { first };
        let last = if last == -1 {
            self.points.len() as i32 - 1
        } else {
            last
        };
        let mut out: Vec<PatrolPathObject> = Vec::new();
        for object in &self.objects {
            let kind = if type_mask != 0 {
                api::space_obj::object_type(cx, object.object_id).unwrap_or(u32::MAX)
            } else {
                u32::MAX
            };
            if kind & type_mask == 0 {
                continue;
            }
            let in_range = object.point_indices.iter().any(|&i| {
                if first <= last {
                    first <= i && i <= last
                } else {
                    first <= i || i <= last
                }
            });
            if in_range && !out.iter().any(|o| o.object_id == object.object_id) {
                out.push(object.clone());
            }
        }
        out
    }

    /// `GetObjectPointIndices`: the point indices of `object`, empty when it
    /// is not registered.
    #[must_use]
    pub fn object_point_indices(&self, object: u32) -> Vec<i32> {
        self.objects
            .iter()
            .find(|o| o.object_id == object)
            .map(|o| o.point_indices.clone())
            .unwrap_or_default()
    }

    /// `GetZoneSegments`: the segments of `zone`'s steps; `None` when the
    /// zone is not on the path.
    #[must_use]
    pub fn zone_segments(&self, zone: u32) -> Option<Vec<PatrolPathSegment>> {
        self.zone_steps.get(&zone).map(|steps| {
            steps
                .iter()
                .map(|&s| PatrolPathSegment {
                    first_point: s,
                    second_point: s + 1,
                })
                .collect()
        })
    }

    /// `GetZoneIds`: the path's zones in ascending order.
    #[must_use]
    pub fn zone_ids(&self) -> Vec<u32> {
        self.zone_steps.keys().copied().collect()
    }
}

/// `FSystemInfo::BuildPatrolPaths` (`SystemInfo.cpp:1114-1209`): the labels
/// sorted by label id (VC6 `std::sort`, which fixes the order of a label's
/// zones), one path per run of equal labels, every zone of a path pointed
/// at it, then up to 32 objects of the system (any distance, type mask
/// `0x600d60`) registered with the paths of the zones within 5000 of them.
/// `labels` is used up, as the C++ erases each run.
pub fn build_patrol_paths(
    cx: &mut Cx<'_>,
    system: u32,
    zones: &mut [SystemZone],
    labels: &mut Vec<ZonePathLabel>,
) -> Vec<PatrolPath> {
    vc6_sort(labels, |a, b| a.label < b.label);
    let mut paths = Vec::new();
    while !labels.is_empty() {
        let label = labels[0].label;
        let last = labels
            .iter()
            .position(|l| l.label != label)
            .unwrap_or(labels.len());
        paths.push(PatrolPath::new(cx, labels, 0, last));
        labels.drain(..last);
    }
    // `ZoneById`: a repeated zone id points at the later zone.
    let mut by_id: BTreeMap<u32, usize> = BTreeMap::new();
    for (index, zone) in zones.iter().enumerate() {
        by_id.insert(zone.zone_id, index);
    }
    for (index, path) in paths.iter().enumerate() {
        for zone in path.zone_ids() {
            if let Some(&z) = by_id.get(&zone) {
                zones[z].path = Some(index);
            }
        }
    }
    let objects = api::system::scan_objects(cx, system, 0x20, Vector::ZERO, -1.0, 0x60_0d60)
        .unwrap_or_default();
    let radius = f64::from(25_000_000.0_f32).sqrt() as f32;
    for object in objects {
        let Ok((position, _)) = api::space_obj::location(cx, object) else {
            continue;
        };
        let near = api::system::in_zones(cx, system, position, radius, 0x4b).unwrap_or_default();
        let mut near_paths: Vec<usize> = Vec::new();
        for zone in near {
            let Some(&z) = by_id.get(&zone) else {
                continue;
            };
            if let Some(p) = zones[z].path
                && !near_paths.contains(&p)
            {
                near_paths.push(p);
            }
        }
        for p in near_paths {
            paths[p].register_object(object, position);
        }
    }
    paths
}
