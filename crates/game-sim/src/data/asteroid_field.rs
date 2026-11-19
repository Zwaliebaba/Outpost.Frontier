//! Asteroid field files (`CAsteroidField::load`), ported from
//! `CppSource/Common/common_casteroidfield.cpp`.
//!
//! A system file's `[Asteroids]` sections name a field file and the zone
//! that holds the field. The field file's `[Cube]` lists the rocks every
//! cube of the field holds (archetype, offset in the cube, orientation),
//! `[Exclusion Zones]` the zones inside the field that are left empty, and
//! `[LootableZone]` the loot of the field or of a zone in it. The server
//! first reads the `[Field]` block (`FSrvAsteroidField::load`) for the cube
//! size, fill distance and empty-cube frequency ([`FieldBlock`]).
//!
//! What the client alone reads (`[Band]`, `[AsteroidBillboards]`,
//! `[DynamicAsteroids]`, `[TexturePanels]`, the colours) belongs to its
//! presentation (P5.6). Loading a rock's model (`Root::load`) is left out
//! with the other engine handles (D117). The cube runtime (population,
//! changes) is [`crate::object::field`].

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::num::{trunc_to_i32, trunc_to_i32_via_i64};

use super::archetype::ArchetypeDb;
use super::asteroid::AxisRotations;
use super::universe::Universe;
use super::zone::exclusion::{AsteroidExclusionZone, AsteroidExclusionZoneMgr};
use super::zone::{Zone, ZoneShape};
use super::{Reports, lossy, value_f32};

const SOURCE: &str = "Common\\CAsteroidSystem.cpp";

/// `kDegToRad`.
const DEG_TO_RAD: f32 = 0.017_453_292;

/// One rock of a cube (`AsteroidCubeArchEntry`).
#[derive(Clone, Debug, PartialEq)]
pub struct CubeArch {
    /// The asteroid archetype's id.
    pub arch: u32,
    /// The offset in the cube: each `asteroid` fraction clamped to -1..1
    /// times half the cube size.
    pub extent: Vector,
    /// The rock's orientation from its three angles (Tier B: sines and
    /// cosines).
    pub orient: Matrix,
    /// The flag `mine`.
    pub mine: bool,
    /// The flag `instant_kill`.
    pub instant_kill: bool,
}

/// The loot of a field or a zone (`LootableZoneEntry`). The asteroid half
/// is never read by the reference: only its defaults remain.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LootableZoneEntry {
    /// Default 0.
    pub asteroid_loot_container: u32,
    /// `dynamic_loot_container`.
    pub dynamic_loot_container: u32,
    /// Default 0.
    pub asteroid_loot_commodity: u32,
    /// `dynamic_loot_commodity`.
    pub dynamic_loot_commodity: u32,
    /// Default 1.
    pub asteroid_loot_count_min: i32,
    /// Default 1.
    pub asteroid_loot_count_max: i32,
    /// `dynamic_loot_count`, the minimum (at least 1, at most the maximum).
    pub dynamic_loot_count_min: i32,
    /// `dynamic_loot_count`, the maximum (at least 1).
    pub dynamic_loot_count_max: i32,
    /// Default 100.
    pub asteroid_loot_difficulty: i32,
    /// `dynamic_loot_difficulty`, 1 to 100.
    pub dynamic_loot_difficulty: i32,
}

impl Default for LootableZoneEntry {
    fn default() -> Self {
        Self {
            asteroid_loot_container: 0,
            dynamic_loot_container: 0,
            asteroid_loot_commodity: 0,
            dynamic_loot_commodity: 0,
            asteroid_loot_count_min: 1,
            asteroid_loot_count_max: 1,
            dynamic_loot_count_min: 1,
            dynamic_loot_count_max: 1,
            asteroid_loot_difficulty: 100,
            dynamic_loot_difficulty: 1,
        }
    }
}

/// The server's first pass over a field file (`FSrvAsteroidField::load`):
/// the `[Field]` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FieldBlock {
    /// `cube_size` (the field's own default when absent).
    pub cube_size: i32,
    /// `fill_dist` (0 when absent).
    pub fill_dist: i32,
    /// `empty_cube_frequency`, truncated to an integer (0 when absent).
    pub empty_cube_frequency: i32,
}

impl FieldBlock {
    /// Every `[Field]` of the file, from where the reader stands, starting
    /// from `cube_size`.
    pub fn read(reader: &mut IniReader, cube_size: i32) -> Self {
        let mut block = Self {
            cube_size,
            fill_dist: 0,
            empty_cube_frequency: 0,
        };
        while reader.read_header() {
            if !reader.is_header(b"Field") {
                continue;
            }
            while reader.read_value() {
                let slot = if reader.is_value(b"cube_size") {
                    &mut block.cube_size
                } else if reader.is_value(b"fill_dist") {
                    &mut block.fill_dist
                } else if reader.is_value(b"empty_cube_frequency") {
                    &mut block.empty_cube_frequency
                } else {
                    continue;
                };
                *slot = trunc_to_i32_via_i64(reader.value_float(0));
            }
        }
        block
    }
}

/// `CAsteroidField`, as far as its file fills it.
#[derive(Clone, Debug, PartialEq)]
pub struct AsteroidField {
    /// The rocks of every cube (`ArchList`), in file order.
    pub arch_list: Vec<CubeArch>,
    /// The cube size the offsets were scaled with (`ArchCubeSize`).
    pub arch_cube_size: i32,
    /// The field's zone (its id), if any.
    pub zone: Option<u32>,
    /// The exclusion zones, made at the first `[Exclusion Zones]`.
    pub exclusion: Option<AsteroidExclusionZoneMgr>,
    /// The system.
    pub system: u32,
    /// The cube rotations (`[Cube] xaxis_rotation` and so on, radians).
    pub rotations: AxisRotations,
    /// The cube size (250).
    pub cube_size: i32,
    /// The fill distance (1000).
    pub fill_dist: i32,
    /// The empty-cube frequency (0).
    pub empty_freq: f32,
    /// The zones that took a `[LootableZone]` by name (`LootZones`).
    pub loot_zones: Vec<u32>,
}

impl Default for AsteroidField {
    fn default() -> Self {
        Self {
            arch_list: Vec::new(),
            arch_cube_size: 0,
            zone: None,
            exclusion: None,
            system: 0,
            rotations: AxisRotations::default(),
            cube_size: 0xfa,
            fill_dist: 1000,
            empty_freq: 0.0,
            loot_zones: Vec::new(),
        }
    }
}

/// What a field load reads besides the file.
pub struct FieldCx<'a> {
    /// The zones (exclusion zones, loot zones), whose loot the load sets.
    pub universe: &'a mut Universe,
    /// The asteroid archetypes.
    pub archetypes: &'a ArchetypeDb,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

/// `value * kDegToRad` stored to a float.
fn radians(value: f64) -> f32 {
    (value * f64::from(DEG_TO_RAD)) as f32
}

/// Clamped to -1..1; NaN stays NaN, as in the C++.
fn clamp_unit(value: f64) -> f64 {
    value.clamp(-1.0, 1.0)
}

/// One rotation step of the rock orientation: rows `a` and `b` turned by
/// `degrees`, with the reference's stores. `minus_first` picks the sign
/// pattern of the x and z steps (`c*a - s*b`, `a*s + c*b`) over that of
/// the y step (`c*a + s*b`, `c*b - a*s`).
fn turn_rows(m: &mut Matrix, a_row: usize, b_row: usize, degrees: f32, minus_first: bool) {
    let r = f64::from(degrees) * f64::from(DEG_TO_RAD);
    let c = f64::from(r.cos() as f32);
    let s = r.sin();
    for col in 0..3 {
        let a = f64::from(m.m[a_row][col]);
        let b = f64::from(m.m[b_row][col]);
        if minus_first {
            m.m[a_row][col] = (c * a - s * b) as f32;
            m.m[b_row][col] = (a * s + c * b) as f32;
        } else {
            m.m[a_row][col] = (c * a + s * b) as f32;
            m.m[b_row][col] = (c * b - a * s) as f32;
        }
    }
}

impl AsteroidField {
    /// `set_zone`.
    pub fn set_zone(&mut self, zone: Option<u32>) {
        self.zone = zone;
    }

    /// `set_system`.
    pub fn set_system(&mut self, system: u32) {
        self.system = system;
    }

    /// `CAsteroidField::load(reader, cube_size)`: every header from where
    /// the reader stands.
    pub fn load(&mut self, reader: &mut IniReader, cube_size: i32, cx: &mut FieldCx<'_>) {
        self.cube_size = cube_size;
        self.arch_cube_size = cube_size;
        while reader.read_header() {
            if reader.is_header(b"Cube") {
                self.read_cube(reader, cx);
            } else if reader.is_header(b"Exclusion Zones") {
                self.read_exclusions(reader, cx);
            } else if reader.is_header(b"LootableZone") {
                self.read_loot(reader, cx);
            }
        }
    }

    fn read_cube(&mut self, reader: &mut IniReader, cx: &mut FieldCx<'_>) {
        while reader.read_value() {
            let axis = if reader.is_value(b"xaxis_rotation") {
                Some(0)
            } else if reader.is_value(b"yaxis_rotation") {
                Some(1)
            } else if reader.is_value(b"zaxis_rotation") {
                Some(2)
            } else {
                None
            };
            if let Some(axis) = axis {
                for index in 0..4u32 {
                    self.rotations.angles[axis][index as usize] =
                        radians(reader.value_float(index));
                }
            } else if reader.is_value(b"asteroid")
                && let Some(entry) = self.read_asteroid(reader, cx)
            {
                self.arch_list.push(entry);
            }
        }
    }

    /// One `asteroid` line. An archetype that is not loaded is reported
    /// and the line dropped (D152).
    fn read_asteroid(&self, reader: &mut IniReader, cx: &mut FieldCx<'_>) -> Option<CubeArch> {
        let name = reader.value_string_at(0);
        let arch = create_id(&name);
        if !cx.archetypes.asteroids.contains_key(&arch) {
            cx.reports.push(
                SOURCE,
                0,
                format!(
                    "asteroid field names asteroid archetype '{}', which is not loaded",
                    lossy(&name)
                ),
            );
            return None;
        }
        let extent_x = clamp_unit(f64::from(value_f32(reader, 1)));
        let extent_y = clamp_unit(f64::from(value_f32(reader, 2)));
        let extent_z = clamp_unit(reader.value_float(3));
        let half = f64::from(self.arch_cube_size) * 0.5;
        let extent = Vector {
            x: (half * extent_x) as f32,
            y: (half * extent_y) as f32,
            z: (half * f64::from(extent_z as f32)) as f32,
        };
        let (mut angle_x, mut angle_y, mut angle_z) = (0.0, 0.0, 0.0);
        if reader.is_value_empty(4) || reader.is_value_empty(5) || reader.is_value_empty(6) {
            cx.reports.push(
                SOURCE,
                0x167,
                "*** ERROR: invalid rotation specified for an asteroid",
            );
        } else {
            angle_y = value_f32(reader, 5);
            angle_x = value_f32(reader, 4);
            angle_z = value_f32(reader, 6);
        }
        let mut orient = Matrix::IDENTITY;
        turn_rows(&mut orient, 1, 2, angle_x, true);
        turn_rows(&mut orient, 0, 2, angle_y, false);
        turn_rows(&mut orient, 0, 1, angle_z, true);
        let (mut mine, mut instant_kill) = (false, false);
        if !reader.is_value_empty(7) {
            let flag = reader.value_string_at(7);
            if flag == b"mine" {
                mine = true;
            } else if flag == b"instant_kill" {
                instant_kill = true;
            }
        }
        Some(CubeArch {
            arch,
            extent,
            orient,
            mine,
            instant_kill,
        })
    }

    /// `[Exclusion Zones]`. A property line before any `exclusion` (or
    /// after one whose zone is missing, which pushes nothing) changes the
    /// previous zone, if any; with none it is skipped (D153). A field without a zone takes its
    /// exclusion zones without the immersion test (D154).
    fn read_exclusions(&mut self, reader: &mut IniReader, cx: &mut FieldCx<'_>) {
        let parent = self.zone.and_then(|id| cx.universe.zones.get(&id)).cloned();
        let mgr = self
            .exclusion
            .get_or_insert_with(AsteroidExclusionZoneMgr::default);
        while reader.read_value() {
            if reader.is_value(b"exclusion") {
                let name = reader.value_string_at(0);
                match cx.universe.zones.get(&create_id(&name)) {
                    None => {
                        cx.reports.push(
                            "Common\\Systems.cpp",
                            0xc9c,
                            format!("*** WARNING: get_zone({}) failed", lossy(&name)),
                        );
                        cx.reports.push(
                            SOURCE,
                            0x18c,
                            format!(
                                "*** ERROR: AsteroidField linked to missing Exclusion Zone ({}) \
                                 in file named ({})",
                                lossy(&name),
                                lossy(reader.file_name())
                            ),
                        );
                    }
                    Some(found) => match &parent {
                        Some(parent) => mgr.push_back(Some(found), parent),
                        None => mgr.zones.push(AsteroidExclusionZone {
                            zone: Some(found.clone()),
                            ..AsteroidExclusionZone::default()
                        }),
                    },
                }
            } else if reader.is_value(b"exclude_billboards") {
                let count = trunc_to_i32(reader.value_float(0));
                if let Some(back) = mgr.back()
                    && count > 0
                {
                    back.exclude_billboards = true;
                }
            } else if reader.is_value(b"billboard_count") {
                let count = trunc_to_i32(reader.value_float(0));
                if let Some(back) = mgr.back()
                    && count > 0
                {
                    back.billboard_count = count as f32;
                }
            } else if reader.is_value(b"exclude_dynamic_asteroids") {
                let count = trunc_to_i32(reader.value_float(0));
                if let Some(back) = mgr.back()
                    && count > 0
                    && back.zone.as_ref().is_some_and(|z| {
                        matches!(z.shape, ZoneShape::Sphere | ZoneShape::Ellipsoid)
                    })
                {
                    back.exclude_dynamic_asteroids = true;
                }
            } else if reader.is_value(b"empty_cube_frequency") {
                let freq = reader.value_float(0);
                if let Some(back) = mgr.back()
                    && freq >= 0.0
                {
                    back.empty_cube_frequency = freq as f32;
                }
            }
        }
    }

    /// `[LootableZone]`: with a dynamic loot line, the entry goes to the
    /// zone named by `zone` (which joins [`AsteroidField::loot_zones`]) or
    /// else to the field's zone; with neither it is reported.
    fn read_loot(&mut self, reader: &mut IniReader, cx: &mut FieldCx<'_>) {
        let mut entry = LootableZoneEntry::default();
        let mut has_dynamic = false;
        let mut named: Option<u32> = None;
        if !reader.read_value() {
            return;
        }
        loop {
            if reader.is_value(b"dynamic_loot_container") {
                entry.dynamic_loot_container = create_id(&reader.value_string_at(0));
                has_dynamic = true;
            } else if reader.is_value(b"dynamic_loot_commodity") {
                entry.dynamic_loot_commodity = create_id(&reader.value_string_at(0));
                has_dynamic = true;
            } else if reader.is_value(b"zone") {
                let name = reader.value_string_at(0);
                let id = create_id(&name);
                named = cx.universe.zones.contains_key(&id).then_some(id);
                if named.is_none() {
                    cx.reports.push(
                        "Common\\Systems.cpp",
                        0xcaa,
                        format!("*** WARNING: get_non_const_zone({}) failed", lossy(&name)),
                    );
                }
            } else if reader.is_value(b"dynamic_loot_count") {
                let mut min = trunc_to_i32(reader.value_float(0));
                let mut max = trunc_to_i32(reader.value_float(1));
                min = min.max(1);
                max = max.max(1);
                if max < min {
                    min = max;
                }
                entry.dynamic_loot_count_min = min;
                entry.dynamic_loot_count_max = max;
            } else if reader.is_value(b"dynamic_loot_difficulty") {
                entry.dynamic_loot_difficulty = trunc_to_i32(reader.value_float(0)).clamp(1, 100);
            }
            if !reader.read_value() {
                break;
            }
        }
        if !has_dynamic {
            return;
        }
        let target = match named {
            Some(id) => {
                self.loot_zones.push(id);
                Some(id)
            }
            None => self.zone,
        };
        match target.and_then(|id| cx.universe.zones.get_mut(&id)) {
            Some(zone) => zone.lootable = Some(entry),
            None => cx.reports.push(
                SOURCE,
                0x246,
                format!(
                    "*** ERROR: loot zone information attached to asteroid field ({}) that has \
                     no zone",
                    lossy(reader.file_name())
                ),
            ),
        }
    }
}

/// The zone a field's loot falls in at `pos` (`get_lootable_zone`): the
/// first loot zone immersed there, else the field's zone when it has loot.
/// The C++ dereferences a missing field zone; here it gives `None` (D154).
#[must_use]
pub fn lootable_zone<'a>(
    field: &AsteroidField,
    universe: &'a Universe,
    pos: Vector,
) -> Option<&'a Zone> {
    for id in &field.loot_zones {
        if let Some(zone) = universe.zones.get(id)
            && zone.immersion(pos) != 0.0
        {
            return Some(zone);
        }
    }
    field
        .zone
        .and_then(|id| universe.zones.get(&id))
        .filter(|zone| zone.lootable.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cube_exclusions_and_loot() {
        let mut universe = Universe::default();
        let mut field_zone = Zone::new(1);
        field_zone.nickname = create_id(b"zf");
        field_zone.shape = ZoneShape::Sphere;
        field_zone.size = Vector::new(1000.0, 0.0, 0.0);
        let mut hole = field_zone.clone();
        hole.nickname = create_id(b"zx");
        hole.size = Vector::new(100.0, 0.0, 0.0);
        let mut loot = hole.clone();
        loot.nickname = create_id(b"zl");
        for z in [field_zone, hole, loot] {
            universe.zones.insert(z.nickname, z);
        }
        let mut archetypes = ArchetypeDb::default();
        archetypes
            .asteroids
            .insert(create_id(b"rock"), Default::default());
        let mut reader = IniReader::new();
        assert!(reader.open_memory(
            b"[Field]\ncube_size = 300\nfill_dist = 900\n\
              [Cube]\nasteroid = rock, 2, -0.5, 0.25, 0, 0, 0, mine\nasteroid = gone, 0, 0, 0, 0, 0, 0\n\
              asteroid = rock, 0, 0, 0\n\
              [Exclusion Zones]\nexclude_billboards = 1\nexclusion = zx\nbillboard_count = 7.9\n\
              exclude_dynamic_asteroids = 1\nempty_cube_frequency = -1\nexclusion = nowhere\n\
              [LootableZone]\nzone = zl\ndynamic_loot_count = 5, 2\ndynamic_loot_difficulty = 400\n\
              dynamic_loot_container = box\n\
              [LootableZone]\ndynamic_loot_count = 3, 4\n"
        ));
        let start = reader.state();
        let block = FieldBlock::read(&mut reader, 250);
        assert_eq!((block.cube_size, block.fill_dist), (300, 900));
        reader.set_state(&start);
        let mut field = AsteroidField::default();
        field.set_zone(Some(create_id(b"zf")));
        let mut reports = Reports::default();
        let mut cx = FieldCx {
            universe: &mut universe,
            archetypes: &archetypes,
            reports: &mut reports,
        };
        field.load(&mut reader, block.cube_size, &mut cx);
        assert_eq!(field.arch_list.len(), 2);
        let first = &field.arch_list[0];
        assert_eq!(first.extent, Vector::new(150.0, -75.0, 37.5));
        assert!(first.mine);
        assert_eq!(first.orient, Matrix::IDENTITY);
        let mgr = field.exclusion.as_ref().expect("exclusions");
        assert_eq!(mgr.zones.len(), 1);
        let x = &mgr.zones[0];
        assert!(!x.exclude_billboards && x.exclude_dynamic_asteroids);
        assert_eq!((x.billboard_count, x.empty_cube_frequency), (7.0, -1.0));
        assert_eq!(field.loot_zones, [create_id(b"zl")]);
        let entry = universe.zones[&create_id(b"zl")].lootable.expect("loot");
        assert_eq!(
            (entry.dynamic_loot_count_min, entry.dynamic_loot_count_max),
            (2, 2)
        );
        assert_eq!(entry.dynamic_loot_difficulty, 100);
        // The second section has no dynamic loot line: nothing changes.
        assert!(universe.zones[&create_id(b"zf")].lootable.is_none());
        let at = lootable_zone(&field, &universe, Vector::new(0.0, 0.0, 0.0));
        assert_eq!(at.map(|z| z.nickname), Some(create_id(b"zl")));
    }
}
