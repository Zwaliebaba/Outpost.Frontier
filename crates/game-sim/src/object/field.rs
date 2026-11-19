//! The asteroid field walk (`CAsteroidField`, `CAsteroidCube`,
//! `CAsteroidSystem::update`), ported from
//! `CppSource/Common/common_casteroidfield.cpp` and
//! `common_casteroid.cpp`, over the field P2.1 loads
//! ([`crate::data::asteroid_field::AsteroidField`]).
//!
//! A field is filled lazily with cubes on its grid (`cube_size`) around
//! each observer: the 2 × 2 × 2 cubes around it and one cube ahead of it
//! ([`FieldRuntime::populate_asteroids`]). A new cube rolls its snap
//! rotation and emptiness from its id ([`compute_cube_orientation`]) and,
//! when filled, places one rock per entry of the field's `[Cube]` list
//! ([`cube_rocks`]). Each walk first drops the cubes that went far in the
//! last one and marks the rest unknown ([`FieldRuntime::flush_changes`]);
//! the cubes no observer touched again then count their time and go near
//! (within 3 s) or far ([`FieldRuntime::update`]). The server reads the
//! changes ([`FieldRuntime::changes`]): the rocks of a new cube get their
//! server objects, those of a far cube lose them.
//!
//! Everything here is a function of the field, its zone and the observer
//! positions; the world ([`crate::object::World`]) makes the rocks'
//! objects and walks the observers (`world::fields`).

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector, mul_matrix, transform_matrix};
use neuron_core::num::trunc_to_i32_via_i64;

use crate::data::asteroid::compute_cube_orientation;
use crate::data::asteroid::{closest_cube_coord, compute_asteroid_id, compute_cube_id};
use crate::data::asteroid_field::{AsteroidField, CubeArch};
use crate::data::zone::Zone;
use crate::data::zone::exclusion::ExcludeObjectType;

use super::ObjHandle;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// A cube's state (`CmnAsteroid::CubeState`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum CubeState {
    /// 0: not touched since the last flush.
    #[default]
    Unknown,
    /// 1: made in this walk.
    Active,
    /// 2: an observer is near, or its timer is at most 3 s.
    Near,
    /// 3: its timer passed 3 s; the next flush drops it.
    Far,
}

impl CubeState {
    /// The C++ value.
    #[must_use]
    pub fn value(self) -> u32 {
        match self {
            Self::Unknown => 0,
            Self::Active => 1,
            Self::Near => 2,
            Self::Far => 3,
        }
    }
}

/// One rock a cube places (`CAsteroidCube::init`'s `CAsteroid::CreateParms`).
#[derive(Clone, Debug, PartialEq)]
pub struct Rock {
    /// The object id: `CreateStaticID` of `"%i%i"` of the cube id and the
    /// rock id (both as signed integers).
    pub id: u32,
    /// The rock id in its cube ([`compute_asteroid_id`] of its offset).
    pub asteroid_id: u32,
    /// The asteroid archetype.
    pub arch: u32,
    /// The world position: the cube's centre plus the offset turned by the
    /// cube.
    pub position: Vector,
    /// The cube's orientation times the entry's (`MulMatrix`).
    pub orientation: Matrix,
    /// A mine.
    pub mine: bool,
    /// Kills on contact.
    pub instant_kill: bool,
}

/// `CAsteroidCube`.
#[derive(Clone, Debug, PartialEq)]
pub struct Cube {
    /// +0x08 the id ([`compute_cube_id`] of its centre).
    pub id: u32,
    /// +0x0c the snap rotation.
    pub orient: Matrix,
    /// +0x30 the centre.
    pub pos: Vector,
    /// +0x3c the field's cube size.
    pub size: i32,
    /// +0x40 the state.
    pub state: CubeState,
    /// +0x00 the time since it was last near an observer.
    pub state_timer: f32,
    /// The field's system.
    pub system: u32,
    /// +0x54 the emptiness draw hit: no rocks.
    pub empty: bool,
    /// The rocks, in the order of the field's `[Cube]` list (an entry whose
    /// offset has no rock id places none).
    pub rocks: Vec<Rock>,
    /// The rocks' objects, parallel to [`Cube::rocks`], once the world made
    /// them (`Asteroids`, +0x48).
    pub objects: Vec<ObjHandle>,
}

/// One entry of `get_changes` (`asteroid_marker`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Marker {
    /// The cube's id.
    pub cube: u32,
    /// The rock's id in its cube.
    pub asteroid_id: u32,
    /// The rock's object id.
    pub object_id: u32,
    /// The cube is new (state 1); otherwise it went far.
    pub cube_active: bool,
    /// A mine.
    pub mine: bool,
    /// Kills on contact.
    pub instant_kill: bool,
}

/// The `TruncWithEpsilon` snap: 0.1 away from zero (towards minus for 0
/// and NaN), truncated through 64 bits.
fn trunc_with_epsilon(v: f64) -> i32 {
    let nudged = if v > 0.0 {
        v + f64::from(0.1_f32)
    } else {
        v - f64::from(0.1_f32)
    };
    trunc_to_i32_via_i64(nudged)
}

/// `ftol(((double)size + 1.0) * 0.5)`.
fn half_size(cube_size: i32) -> i32 {
    trunc_to_i32_via_i64((f64::from(cube_size) + 1.0) * 0.5)
}

/// The rocks of a filled cube (`CAsteroidCube::init`'s spawn loop): for
/// each entry, its offset turned by the cube (`TransformMatrix`), the rock
/// id of that offset (no rock when it has none), the object id from the
/// name `"%i%i"`, the position and the orientation (`MulMatrix`).
#[must_use]
pub fn cube_rocks(cube_id: u32, pos: Vector, orient: &Matrix, arch_list: &[CubeArch]) -> Vec<Rock> {
    let mut out = Vec::new();
    for entry in arch_list {
        let offset = transform_matrix(orient, entry.extent);
        let Some(asteroid_id) = compute_asteroid_id([offset.x, offset.y, offset.z]) else {
            continue;
        };
        let name = format!("{}{}", cube_id.cast_signed(), asteroid_id.cast_signed());
        out.push(Rock {
            id: create_id(name.as_bytes()),
            asteroid_id,
            arch: entry.arch,
            position: Vector::new(pos.x + offset.x, pos.y + offset.y, pos.z + offset.z),
            orientation: mul_matrix(orient, &entry.orient),
            mine: entry.mine,
            instant_kill: entry.instant_kill,
        });
    }
    out
}

/// A field with its cubes (`CAsteroidField`'s runtime part).
#[derive(Clone, Debug, PartialEq)]
pub struct FieldRuntime {
    /// The field as its file filled it.
    pub field: AsteroidField,
    /// The cubes by id (`Cubes`, a `std::map`).
    pub cubes: BTreeMap<u32, Cube>,
    /// The server's record of the rocks it made objects for
    /// (`FSrvAsteroidField::Cubes`): cube id → rock id → object.
    pub server: BTreeMap<u32, BTreeMap<u32, ObjHandle>>,
}

impl FieldRuntime {
    /// A field without cubes.
    #[must_use]
    pub fn new(field: AsteroidField) -> Self {
        Self {
            field,
            cubes: BTreeMap::new(),
            server: BTreeMap::new(),
        }
    }

    /// `CAsteroidField::near_field`: whether `pos` is within the fill
    /// margin (`(cube_size + fill_dist) * 1.74`) of the zone's surface;
    /// true without a zone.
    #[must_use]
    pub fn near_field(&self, zone: Option<&Zone>, pos: Vector) -> bool {
        let Some(zone) = zone else {
            return true;
        };
        let f = &self.field;
        let margin = r(f64::from(f.cube_size.wrapping_add(f.fill_dist)) * e(1.74_f32));
        let delta = Vector::new(pos.x - zone.pos.x, pos.y - zone.pos.y, pos.z - zone.pos.z);
        let reach = e(zone.radius()) + e(margin);
        let dist_sq_wide =
            (e(delta.x) * e(delta.x) + e(delta.z) * e(delta.z)) + e(delta.y) * e(delta.y);
        let dist_sq = r(dist_sq_wide);
        if dist_sq_wide > reach * reach {
            return false;
        }
        let dist_wide = e(dist_sq).sqrt();
        let dist = r(dist_wide);
        let excess = if dist_wide > e(0.0001_f32) {
            let inv = 1.0 / e(dist);
            let dir = Vector::new(
                r(e(delta.x) * inv),
                r(e(delta.y) * inv),
                r(e(delta.z) * inv),
            );
            e(dist) - zone.radius_along(dir)
        } else {
            0.0
        };
        excess <= e(margin)
    }

    /// `CAsteroidField::near_field_fraction`: 1 deep inside, falling to 0
    /// at `dist` outside the zone's surface (`dist` not above 0: the fill
    /// margin `(fill_dist + cube_size) * 1.2`); 1 without a zone.
    #[must_use]
    pub fn near_field_fraction(&self, zone: Option<&Zone>, pos: Vector, dist: f32) -> f32 {
        let Some(zone) = zone else {
            return 1.0;
        };
        let f = &self.field;
        let dist = if dist > 0.0 {
            dist
        } else {
            r(f64::from(f.fill_dist.wrapping_add(f.cube_size)) * e(1.2_f32))
        };
        let delta = Vector::new(pos.x - zone.pos.x, pos.y - zone.pos.y, pos.z - zone.pos.z);
        let reach = e(zone.radius()) + e(dist);
        let dist_sq_wide =
            (e(delta.x) * e(delta.x) + e(delta.z) * e(delta.z)) + e(delta.y) * e(delta.y);
        let dist_sq = r(dist_sq_wide);
        if dist_sq_wide > reach * reach {
            return 0.0;
        }
        let len_wide = e(dist_sq).sqrt();
        let len = r(len_wide);
        let inv = 1.0 / len_wide;
        let dir = Vector::new(
            r(e(delta.x) * inv),
            r(e(delta.y) * inv),
            r(e(delta.z) * inv),
        );
        let excess = e(len) - zone.radius_along(dir);
        if !(excess <= e(dist)) {
            return 0.0;
        }
        r(1.0 - excess / e(dist))
    }

    /// The grid snap of `closest_cube_pos` and `create_cube_id`.
    fn snap(&self, x: f64, y: f64, z: f64) -> Vector {
        let size = self.field.cube_size;
        let half = half_size(size);
        let c = |v: f64| closest_cube_coord(trunc_with_epsilon(v), size, half) as f32;
        Vector::new(c(x), c(y), c(z))
    }

    /// `CAsteroidField::closest_cube_pos`: the centre of the field's cube
    /// that holds `pos`.
    #[must_use]
    pub fn closest_cube_pos(&self, pos: Vector) -> Vector {
        self.snap(e(pos.x), e(pos.y), e(pos.z))
    }

    /// `CAsteroidField::create_cube_id`: [`compute_cube_id`] of
    /// [`FieldRuntime::closest_cube_pos`].
    #[must_use]
    pub fn create_cube_id(&self, pos: Vector) -> u32 {
        let c = self.closest_cube_pos(pos);
        compute_cube_id([c.x, c.y, c.z])
    }

    /// `CAsteroidField::find_cube`.
    #[must_use]
    pub fn find_cube(&self, id: u32) -> Option<&Cube> {
        self.cubes.get(&id)
    }

    /// `CAsteroidField::populate_asteroids(pos, dir)`: the 2 × 2 × 2 grid
    /// cubes around `pos` (towards the side of the cube centre `pos` is
    /// on), within `0.99 × cube_size`, then the cube 1.25 cube sizes along
    /// `dir`, within 1000 m. `zone` is the field's zone, which the C++
    /// reads without a null test. Returns the ids of the cubes made, in
    /// the order they were made.
    pub fn populate_asteroids(&mut self, zone: &Zone, pos: Vector, dir: Vector) -> Vec<u32> {
        let mut made = Vec::new();
        let grid = self.closest_cube_pos(pos);
        let size = self.field.cube_size;
        let scaled = f64::from(size) * 0.99;
        let grid_threshold = r(scaled * scaled);
        let offset_x = if pos.x > grid.x {
            size
        } else {
            size.wrapping_neg()
        };
        let offset_y = if pos.y > grid.y {
            size
        } else {
            size.wrapping_neg()
        };
        let offset_z = if pos.z > grid.z {
            size
        } else {
            size.wrapping_neg()
        };
        for cz in 0..2 {
            for cy in 0..2 {
                for cx in 0..2 {
                    let pick = |set: bool, offset: i32, g: f32| {
                        if set { r(f64::from(offset) + e(g)) } else { g }
                    };
                    let candidate = Vector::new(
                        pick(cx != 0, offset_x, grid.x),
                        pick(cy != 0, offset_y, grid.y),
                        pick(cz != 0, offset_z, grid.z),
                    );
                    if let Some(id) = self.populate_internal(zone, pos, candidate, grid_threshold) {
                        made.push(id);
                    }
                }
            }
        }
        // The cube ahead: the x term rounded once, y's product stored and
        // the rest kept, z stored at each step.
        let c = f64::from(size);
        let ahead_x = r(c * e(dir.x) * 1.25 + e(pos.x));
        let scaled_y = r(c * e(dir.y));
        let ahead_y = e(scaled_y) * 1.25 + e(pos.y);
        let scaled_z = r(c * e(dir.z));
        let scaled_z125 = r(e(scaled_z) * 1.25);
        let ahead_z = r(e(scaled_z125) + e(pos.z));
        let candidate = self.snap(e(ahead_x), ahead_y, e(ahead_z));
        if let Some(id) = self.populate_internal(zone, pos, candidate, 1_000_000.0) {
            made.push(id);
        }
        made
    }

    /// `populate_asteroids_internal`: the cube at `candidate` unless it is
    /// excluded or has no id; an existing cube that is neither new nor near
    /// turns near when `pos` is within the threshold; a missing one is
    /// made there when the field's zone covers the candidate. The made
    /// cube's id.
    fn populate_internal(
        &mut self,
        zone: &Zone,
        pos: Vector,
        candidate: Vector,
        dist_sq_threshold: f32,
    ) -> Option<u32> {
        let mut freq = self.field.empty_freq;
        let mut not_excluded = true;
        if let Some(mgr) = &self.field.exclusion {
            let mut out = -1.0_f32;
            not_excluded = !mgr.is_excluded(candidate, ExcludeObjectType::CmnAsteroid, &mut out);
            if not_excluded && out >= 0.0 {
                freq = out;
            }
        }
        let id = compute_cube_id([candidate.x, candidate.y, candidate.z]);
        if id == 0 || !not_excluded {
            return None;
        }
        let delta = Vector::new(
            pos.x - candidate.x,
            pos.y - candidate.y,
            pos.z - candidate.z,
        );
        let dist_sq = (e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y)) + e(delta.x) * e(delta.x);
        if let Some(cube) = self.cubes.get_mut(&id) {
            if matches!(cube.state, CubeState::Active | CubeState::Near) {
                return None;
            }
            if dist_sq < e(dist_sq_threshold) {
                cube.state = CubeState::Near;
                cube.state_timer = 0.0;
            }
            return None;
        }
        if !(dist_sq < e(dist_sq_threshold)) {
            return None;
        }
        if zone.immersion(candidate) == 0.0 {
            return None;
        }
        let mut orient = Matrix::IDENTITY;
        let empty = !compute_cube_orientation(id, &mut orient, &self.field.rotations, freq);
        let rocks = if empty {
            Vec::new()
        } else {
            cube_rocks(id, candidate, &orient, &self.field.arch_list)
        };
        self.cubes.insert(
            id,
            Cube {
                id,
                orient,
                pos: candidate,
                size: self.field.cube_size,
                state: CubeState::Active,
                state_timer: 0.0,
                system: self.field.system,
                empty,
                rocks,
                objects: Vec::new(),
            },
        );
        Some(id)
    }

    /// `CAsteroidField::update(dt)`: every cube still unknown counts `dt`
    /// and turns far past 3 s, near otherwise.
    pub fn update(&mut self, dt: f32) {
        for cube in self.cubes.values_mut() {
            if cube.state == CubeState::Unknown {
                cube.state_timer += dt;
                cube.state = if cube.state_timer > 3.0 {
                    CubeState::Far
                } else {
                    CubeState::Near
                };
            }
        }
    }

    /// `CAsteroidField::flush_changes`: the far cubes are dropped (returned
    /// in id order, with their rocks' objects); every other cube becomes
    /// unknown.
    pub fn flush_changes(&mut self) -> Vec<Cube> {
        let far: Vec<u32> = self
            .cubes
            .iter()
            .filter(|(_, c)| c.state == CubeState::Far)
            .map(|(id, _)| *id)
            .collect();
        for cube in self.cubes.values_mut() {
            if cube.state != CubeState::Far {
                cube.state = CubeState::Unknown;
            }
        }
        far.into_iter()
            .filter_map(|id| self.cubes.remove(&id))
            .collect()
    }

    /// `CAsteroidField::get_changes`: one marker per rock of every filled
    /// cube that is new or far, cubes in id order.
    #[must_use]
    pub fn changes(&self) -> Vec<Marker> {
        let mut out = Vec::new();
        for cube in self.cubes.values() {
            if cube.empty || !matches!(cube.state, CubeState::Active | CubeState::Far) {
                continue;
            }
            for rock in &cube.rocks {
                out.push(Marker {
                    cube: cube.id,
                    asteroid_id: rock.asteroid_id,
                    object_id: rock.id,
                    cube_active: cube.state == CubeState::Active,
                    mine: rock.mine,
                    instant_kill: rock.instant_kill,
                });
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::zone::ZoneShape;

    fn sphere(radius: f32) -> Zone {
        let mut z = Zone::new(1);
        z.shape = ZoneShape::Sphere;
        z.size = Vector::new(radius, 0.0, 0.0);
        z
    }

    fn rock_entry(x: f32, mine: bool) -> CubeArch {
        CubeArch {
            arch: 7,
            extent: Vector::new(x, 0.0, 0.0),
            orient: Matrix::IDENTITY,
            mine,
            instant_kill: false,
        }
    }

    #[test]
    fn cubes_appear_go_far_and_drop() {
        let zone = sphere(5000.0);
        let field = AsteroidField {
            cube_size: 400,
            fill_dist: 600,
            empty_freq: -1.0,
            arch_list: vec![rock_entry(100.0, false), rock_entry(-50.0, true)],
            ..AsteroidField::default()
        };
        let mut f = FieldRuntime::new(field);
        let pos = Vector::new(10.0, 20.0, 30.0);
        assert!(f.near_field(Some(&zone), pos));
        assert!(!f.near_field(Some(&zone), Vector::new(9000.0, 0.0, 0.0)));
        let made = f.populate_asteroids(&zone, pos, Vector::new(0.0, 0.0, -1.0));
        assert!(!made.is_empty());
        assert!(f.cubes.values().all(|c| c.state == CubeState::Active));
        let markers = f.changes();
        assert_eq!(markers.len(), made.len() * 2);
        assert!(markers.iter().all(|m| m.cube_active));
        // A second walk with the observer gone: unknown, then near until
        // the timer passes 3 s, then far, then dropped.
        assert!(f.flush_changes().is_empty());
        f.update(2.0);
        assert!(f.cubes.values().all(|c| c.state == CubeState::Near));
        assert!(f.changes().is_empty());
        assert!(f.flush_changes().is_empty());
        f.update(1.5);
        assert!(f.cubes.values().all(|c| c.state == CubeState::Far));
        assert!(f.changes().iter().all(|m| !m.cube_active));
        assert_eq!(f.flush_changes().len(), made.len());
        assert!(f.cubes.is_empty());
    }

    #[test]
    fn rock_names_are_signed() {
        let rocks = cube_rocks(
            0x8000_0001,
            Vector::ZERO,
            &Matrix::IDENTITY,
            &[rock_entry(-4.0, false)],
        );
        assert_eq!(rocks.len(), 1);
        let id = rocks[0].asteroid_id;
        let name = format!("{}{}", 0x8000_0001_u32.cast_signed(), id.cast_signed());
        assert!(name.starts_with('-'));
        assert_eq!(rocks[0].id, create_id(name.as_bytes()));
        assert_eq!(rocks[0].position, Vector::new(-4.0, 0.0, 0.0));
    }

    #[test]
    fn snaps_follow_the_reference() {
        let field = AsteroidField {
            cube_size: 250,
            ..AsteroidField::default()
        };
        let f = FieldRuntime::new(field);
        assert_eq!(
            f.closest_cube_pos(Vector::new(10.0, -10.0, 300.0)),
            Vector::new(125.0, -125.0, 375.0)
        );
        assert_eq!(
            f.create_cube_id(Vector::new(10.0, -10.0, 300.0)),
            compute_cube_id([125.0, -125.0, 375.0])
        );
    }
}
