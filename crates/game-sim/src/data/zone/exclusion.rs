//! Exclusion zones (`Exclusion::AsteroidExclusionZone`,
//! `NebulaExclusionZone` and their managers), ported from
//! `CppSource/Common/common_exclusionzone.cpp`.
//!
//! An asteroid field or a nebula lists zones inside it where its contents
//! (asteroids, billboards, dynamic asteroids, fog) are left out. A manager
//! keeps them sorted by their distance from an observer, nearest first,
//! with the VC6 `std::sort`, so that the queries can stop at the first zone
//! beyond a cut-off. The C++ managers hold pointers to the universe's zones;
//! here each exclusion zone holds a copy of its zone, which never changes
//! after the load.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true exactly
// when the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::id::create_id;
use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32_via_i64;

use super::{Zone, ZoneShape};
use crate::data::Reports;
use crate::data::vc6::vc6_sort;

const SOURCE: &str = "Common\\ExclusionZoneMgr.cpp";

/// The "far away" squared distance a new exclusion zone starts with.
const FAR: f32 = 10_000_000.0;

/// What an asteroid exclusion query asks about (`ExcludeObjectType`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExcludeObjectType {
    /// `EOT_CliAsteroid` (0): client asteroids, within the distance cut-off.
    CliAsteroid,
    /// `EOT_CmnAsteroid` (1): asteroids.
    CmnAsteroid,
    /// `EOT_Billboard` (2): billboards.
    Billboard,
    /// `EOT_DynamicAsteroid` (3): dynamic asteroids.
    DynamicAsteroid,
    /// 4: nebula exclusion (the nebula manager's only type).
    Nebula,
    /// Any other value.
    Other(i32),
}

impl ExcludeObjectType {
    /// The type of a C++ value.
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            0 => Self::CliAsteroid,
            1 => Self::CmnAsteroid,
            2 => Self::Billboard,
            3 => Self::DynamicAsteroid,
            4 => Self::Nebula,
            other => Self::Other(other),
        }
    }
}

/// The zone's immersion at `pos`, 0 without a zone (see D154).
fn immersion(zone: Option<&Zone>, pos: Vector) -> f64 {
    zone.map_or(0.0, |z| z.immersion(pos))
}

/// `AsteroidExclusionZone`.
#[derive(Clone, Debug, PartialEq)]
pub struct AsteroidExclusionZone {
    /// The zone (`ZonePtr`), `None` when set to null.
    pub zone: Option<Zone>,
    /// The squared distance of its surface from the last observer.
    pub max_dist_sq: f32,
    /// `exclude_billboards`.
    pub exclude_billboards: bool,
    /// `exclude_dynamic_asteroids`.
    pub exclude_dynamic_asteroids: bool,
    /// `empty_cube_frequency` (-1 for none).
    pub empty_cube_frequency: f32,
    /// `billboard_count` (-1 for none), a density once normalised.
    pub billboard_count: f32,
}

impl Default for AsteroidExclusionZone {
    fn default() -> Self {
        Self {
            zone: None,
            max_dist_sq: FAR,
            exclude_billboards: false,
            exclude_dynamic_asteroids: false,
            empty_cube_frequency: -1.0,
            billboard_count: -1.0,
        }
    }
}

impl AsteroidExclusionZone {
    /// `set_zone`: the zone is kept either way; true when `parent` is
    /// immersed at the zone's centre.
    pub fn set_zone(&mut self, zone: Option<&Zone>, parent: &Zone) -> bool {
        self.zone = zone.cloned();
        match zone {
            Some(z) => parent.immersion(z.pos) > 0.0,
            None => false,
        }
    }

    /// `update_dist`: the squared distance from `pos` to the zone's surface
    /// (0 inside) for a sphere or an ellipsoid; 0 for every other shape.
    pub fn update_dist(&mut self, pos: Vector) {
        let Some(zone) = &self.zone else {
            return;
        };
        let dx = pos.x - zone.pos.x;
        let dy = pos.y - zone.pos.y;
        let dz = pos.z - zone.pos.z;
        let (dx64, dy64, dz64) = (f64::from(dx), f64::from(dy), f64::from(dz));
        let (length, radius) = match zone.shape {
            ZoneShape::Sphere => {
                let length = ((dz64 * dz64 + dy64 * dy64) + dx64 * dx64).sqrt() as f32;
                (length, f64::from(zone.radius()))
            }
            ZoneShape::Ellipsoid => {
                let length = ((dz64 * dz64 + dx64 * dx64) + dy64 * dy64).sqrt() as f32;
                let inv = 1.0 / f64::from(length);
                let dir = Vector {
                    x: (dx64 * inv) as f32,
                    y: (dy64 * inv) as f32,
                    z: (dz64 * inv) as f32,
                };
                (length, zone.radius_along(dir))
            }
            _ => {
                self.max_dist_sq = 0.0;
                return;
            }
        };
        let dist = f64::from(length) - radius;
        self.max_dist_sq = if dist < 0.0 {
            0.0
        } else {
            (dist * dist) as f32
        };
    }
}

/// `AsteroidExclusionZoneMgr`.
#[derive(Clone, Debug, PartialEq)]
pub struct AsteroidExclusionZoneMgr {
    /// The zones, in push order until the first sort.
    pub zones: Vec<AsteroidExclusionZone>,
    /// The client-asteroid cut-off, squared (2000 squared).
    pub max_cli_dist: f32,
    /// The dynamic-asteroid cut-off, squared (500 squared).
    pub max_dyn_dist: f32,
}

impl Default for AsteroidExclusionZoneMgr {
    fn default() -> Self {
        Self {
            zones: Vec::new(),
            max_cli_dist: 4_000_000.0,
            max_dyn_dist: 250_000.0,
        }
    }
}

impl AsteroidExclusionZoneMgr {
    /// `push_back`: a new exclusion zone on `zone` inside `parent`.
    pub fn push_back(&mut self, zone: Option<&Zone>, parent: &Zone) {
        let mut entry = AsteroidExclusionZone::default();
        entry.set_zone(zone, parent);
        self.zones.push(entry);
    }

    /// `back`: the last zone pushed.
    pub fn back(&mut self) -> Option<&mut AsteroidExclusionZone> {
        self.zones.last_mut()
    }

    /// `sort`: the distances from `observer`, then the VC6 sort by them.
    pub fn sort(&mut self, observer: Vector) {
        for zone in &mut self.zones {
            zone.update_dist(observer);
        }
        vc6_sort(&mut self.zones, |a, b| a.max_dist_sq < b.max_dist_sq);
    }

    /// `get_max_billboard_count`: the largest positive whole billboard
    /// count, or -1.
    #[must_use]
    pub fn max_billboard_count(&self) -> i32 {
        let mut max = -1;
        for zone in &self.zones {
            let count = trunc_to_i32_via_i64(f64::from(zone.billboard_count));
            if count > 0 && count > max {
                max = count;
            }
        }
        max
    }

    /// `normalize_billboard_density`: each positive whole count divided by
    /// `total`.
    pub fn normalize_billboard_density(&mut self, total: i32) {
        let inverse = (1.0 / f64::from(total)) as f32;
        for zone in &mut self.zones {
            let count = trunc_to_i32_via_i64(f64::from(zone.billboard_count));
            if f64::from(count) > 0.0 {
                zone.billboard_count = (f64::from(count) * f64::from(inverse)) as f32;
            }
        }
    }

    /// `set_max_cliasteroid_exclusion_distance`.
    pub fn set_max_cliasteroid_exclusion_distance(&mut self, dist: f32) {
        self.max_cli_dist = dist * dist;
    }

    /// `set_max_dynamicasteroid_exclusion_distance`.
    pub fn set_max_dynamicasteroid_exclusion_distance(&mut self, dist: f32) {
        self.max_dyn_dist = dist * dist;
    }

    /// `is_excluded(pos, kind, out)`. `out` is written as the C++ writes
    /// it (not at all for some answers).
    pub fn is_excluded(&self, pos: Vector, kind: ExcludeObjectType, out: &mut f32) -> bool {
        match kind {
            ExcludeObjectType::CliAsteroid => self.asteroid_excluded(pos, out, true),
            ExcludeObjectType::CmnAsteroid => self.asteroid_excluded(pos, out, false),
            ExcludeObjectType::Billboard => self.billboard_excluded(pos, out),
            ExcludeObjectType::DynamicAsteroid => self.dynamic_asteroid_excluded(pos),
            _ => false,
        }
    }

    /// `is_cliasteroid_excluded` (with the cut-off) and
    /// `is_cmnasteroid_excluded`.
    fn asteroid_excluded(&self, pos: Vector, out: &mut f32, cut_off: bool) -> bool {
        *out = -1.0;
        for zone in &self.zones {
            if cut_off && zone.max_dist_sq > self.max_cli_dist {
                return false;
            }
            if immersion(zone.zone.as_ref(), pos) != 0.0 {
                if !(zone.empty_cube_frequency >= 0.0) {
                    return true;
                }
                *out = zone.empty_cube_frequency;
            }
        }
        false
    }

    /// `is_billboard_excluded`: true only for a billboard-excluding zone;
    /// otherwise the density of the first zone with one that holds `pos`
    /// goes to `out`.
    fn billboard_excluded(&self, pos: Vector, out: &mut f32) -> bool {
        let mut found = false;
        for zone in &self.zones {
            if zone.exclude_billboards {
                if immersion(zone.zone.as_ref(), pos) != 0.0 {
                    return true;
                }
                continue;
            }
            let density = zone.billboard_count;
            if found || !(density > 0.0) {
                continue;
            }
            if immersion(zone.zone.as_ref(), pos) != 0.0 {
                found = true;
                *out = density;
            }
        }
        false
    }

    /// `is_dynamic_asteroid_excluded`.
    fn dynamic_asteroid_excluded(&self, pos: Vector) -> bool {
        for zone in &self.zones {
            if zone.max_dist_sq > self.max_dyn_dist {
                return false;
            }
            if zone.exclude_dynamic_asteroids && immersion(zone.zone.as_ref(), pos) != 0.0 {
                return true;
            }
        }
        false
    }
}

/// `NebulaExclusionZone`.
#[derive(Clone, Debug, PartialEq)]
pub struct NebulaExclusionZone {
    /// The zone.
    pub zone: Zone,
    /// The squared distance of its surface from the last observer.
    pub max_dist_sq: f32,
    /// `fog_far` (-1 for none).
    pub fog_far: f32,
    /// `zone_shell`: `CreateID` of the shell mesh name (0 for none).
    pub zone_shell_name: u32,
}

impl NebulaExclusionZone {
    /// `NebulaExclusionZone()` with `set_zone(zone, parent)`: reports a zone
    /// that is not a sphere or not wholly inside the nebula; returns the
    /// zone and whether the nebula is immersed at its centre.
    pub fn new(zone: &Zone, parent: &Zone, reports: &mut Reports) -> (Self, bool) {
        if zone.shape != ZoneShape::Sphere {
            reports.push(
                SOURCE,
                0x1e9,
                format!(
                    "*** ERROR: DATA ERROR: nebula exclusion zones may only be SPHERE shaped\n \
                     {} is not a sphere zone",
                    zone.nickname
                ),
            );
        }
        let value = parent.immersion(zone.pos);
        let inside = value as f32;
        if !(value >= 1.0) {
            reports.push(
                SOURCE,
                0x1f5,
                format!(
                    "*** ERROR: DATA ERROR: nebula exclusion zone ({:#x}) is not fully inside \
                     nebula zone ({:#x})",
                    zone.nickname, parent.nickname
                ),
            );
        }
        (
            Self {
                zone: zone.clone(),
                max_dist_sq: FAR,
                fog_far: -1.0,
                zone_shell_name: 0,
            },
            inside > 0.0,
        )
    }

    /// `update_dist` (the member; the manager's sort has its own copy).
    pub fn update_dist(&mut self, pos: Vector) {
        let dx = f64::from(pos.x) - f64::from(self.zone.pos.x);
        let dy = f64::from(pos.y) - f64::from(self.zone.pos.y);
        let dz = f64::from(pos.z) - f64::from(self.zone.pos.z);
        let length = ((dz * dz + dx * dx) + dy * dy).sqrt() as f32;
        let dist = f64::from(length) - f64::from(self.zone.radius());
        self.max_dist_sq = if dist < 0.0 {
            0.0
        } else {
            (dist * dist) as f32
        };
    }

    /// `set_zone_shell_name`.
    pub fn set_zone_shell_name(&mut self, name: &[u8]) {
        self.zone_shell_name = create_id(name);
    }
}

/// `NebulaExclusionZoneMgr`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NebulaExclusionZoneMgr {
    /// The zones.
    pub zones: Vec<NebulaExclusionZone>,
    /// `set_max_distance`, squared (0 until set; see D155).
    pub max_dist: f32,
}

impl NebulaExclusionZoneMgr {
    /// `push_back`: also reports a zone without an edge fraction and one
    /// not inside the nebula.
    pub fn push_back(&mut self, zone: &Zone, parent: &Zone, reports: &mut Reports) {
        let (entry, inside) = NebulaExclusionZone::new(zone, parent, reports);
        self.zones.push(entry);
        if !(zone.edge_fraction > 0.0) {
            reports.push(
                SOURCE,
                0x170,
                format!(
                    "*** ERROR: DATA ERROR: edge fraction must be added to exclusion zone {} \
                     inorder for transitions to work",
                    zone.nickname
                ),
            );
        }
        if !inside {
            reports.push(
                SOURCE,
                0x176,
                format!(
                    "*** ERROR: DATA ERROR: exclusion zone {} not inside nebula",
                    zone.nickname
                ),
            );
        }
    }

    /// `back`.
    pub fn back(&mut self) -> Option<&mut NebulaExclusionZone> {
        self.zones.last_mut()
    }

    /// `sort`, with its own distance arithmetic.
    pub fn sort(&mut self, observer: Vector) {
        for zone in &mut self.zones {
            let p = zone.zone.pos;
            let dx = f64::from(observer.x - p.x);
            let dy = f64::from(observer.y - p.y);
            let dz = f64::from(observer.z - p.z);
            let length = ((dy * dy + dz * dz) + dx * dx).sqrt() as f32;
            let dist = f64::from(length) - f64::from(zone.zone.radius());
            zone.max_dist_sq = if dist < 0.0 {
                0.0
            } else {
                (dist * dist) as f32
            };
        }
        vc6_sort(&mut self.zones, |a, b| a.max_dist_sq < b.max_dist_sq);
    }

    /// `is_excluded`: type 4 only, true inside any zone.
    #[must_use]
    pub fn is_excluded(&self, pos: Vector, kind: ExcludeObjectType) -> bool {
        kind == ExcludeObjectType::Nebula && self.zones.iter().any(|z| z.zone.immersion(pos) != 0.0)
    }

    /// `is_in_exclusion_zone`: the first zone immersed above 1e-5 at `pos`
    /// (by index) and that immersion; with `near_only` the scan stops at
    /// the first zone farther than 10 (squared).
    #[must_use]
    pub fn is_in_exclusion_zone(&self, pos: Vector, near_only: bool) -> Option<(usize, f32)> {
        for (index, zone) in self.zones.iter().enumerate() {
            if near_only && zone.max_dist_sq > 10.0 {
                return None;
            }
            let inside = zone.zone.immersion(pos);
            if inside > f64::from(1.0e-5_f32) {
                return Some((index, inside as f32));
            }
        }
        None
    }

    /// `set_max_distance`.
    pub fn set_max_distance(&mut self, dist: f32) {
        self.max_dist = dist * dist;
    }
}
