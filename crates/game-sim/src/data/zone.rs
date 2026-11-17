//! Zones (`ZONE`, `common_zone.cpp`): the record the universe loader reads
//! from a system file's `[Zone]` sections, and the queries on it that do
//! not need the shape geometry.
//!
//! The membership tests, distances, radius along a direction and surface
//! point are in [`shape`]; `ClassifyPoint`, immersion, the segment clips
//! (`Intersect`) and the frustum and sphere culls in [`query`]; the
//! exclusion zones of asteroid fields and nebulae in [`exclusion`].

pub mod exclusion;
pub mod query;
pub mod shape;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::{create_id, make_id};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::string_cache::{CacheString, StringCache};
use super::{Reports, lossy, value_f32};

/// The C++ source the zone messages name.
const SOURCE: &str = "Common\\Systems.cpp";

/// `EZoneShape`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ZoneShape {
    /// No `shape`, or an unknown name (0).
    #[default]
    Invalid,
    /// `sphere` (1).
    Sphere,
    /// `ellipsoid` (2).
    Ellipsoid,
    /// `BOX` (3).
    Box,
    /// `CYLINDER` (4).
    Cylinder,
    /// `RING` (5).
    Ring,
    /// `MESH` (6).
    Mesh,
}

impl ZoneShape {
    /// The C++ value.
    #[must_use]
    pub fn value(self) -> u32 {
        match self {
            Self::Invalid => 0,
            Self::Sphere => 1,
            Self::Ellipsoid => 2,
            Self::Box => 3,
            Self::Cylinder => 4,
            Self::Ring => 5,
            Self::Mesh => 6,
        }
    }

    /// `ParseZoneShape`: an unknown name is reported and invalid.
    pub fn parse(text: &[u8], reports: &mut Reports) -> Self {
        const NAMES: [(&[u8], ZoneShape); 6] = [
            (b"sphere", ZoneShape::Sphere),
            (b"ellipsoid", ZoneShape::Ellipsoid),
            (b"BOX", ZoneShape::Box),
            (b"CYLINDER", ZoneShape::Cylinder),
            (b"RING", ZoneShape::Ring),
            (b"MESH", ZoneShape::Mesh),
        ];
        if let Some(&(_, shape)) = NAMES.iter().find(|(n, _)| eq_ignore_case(text, n)) {
            return shape;
        }
        reports.push(
            SOURCE,
            0x24c,
            format!("*** WARNING: invalid ZoneShape '{}'", lossy(text)),
        );
        Self::Invalid
    }
}

/// `EulerMatrix`: rotation angles in degrees (x pitch, y yaw, z roll) to a
/// matrix, in the reference's operation order. The sines and cosines come
/// from the Rust standard library, so the result is Tier B (decision T11).
#[must_use]
pub fn euler_matrix(degrees: Vector) -> Matrix {
    let k = 0.017_453_292_f32;
    let pitch = f64::from(degrees.x * k);
    let yaw = f64::from(degrees.y * k);
    let roll = f64::from(degrees.z * k);
    let (sin_p, cos_p) = (pitch.sin(), pitch.cos());
    let (sin_y, cos_y) = (yaw.sin(), yaw.cos());
    let (sin_r, cos_r) = (roll.sin(), roll.cos());
    let sin_y_sin_p = sin_y * sin_p;
    let sin_r_cos_p = f64::from((sin_r * cos_p) as f32);
    let cos_r_cos_p = f64::from((cos_r * cos_p) as f32);
    Matrix::from_rows([
        [
            (cos_r * cos_y) as f32,
            (sin_y_sin_p * cos_r - sin_r_cos_p) as f32,
            (cos_r_cos_p * sin_y + sin_r * sin_p) as f32,
        ],
        [
            (sin_r * cos_y) as f32,
            (sin_y_sin_p * sin_r + cos_r_cos_p) as f32,
            (sin_r_cos_p * sin_y - cos_r * sin_p) as f32,
        ],
        [
            (-sin_y) as f32,
            (cos_y * sin_p) as f32,
            (cos_y * cos_p) as f32,
        ],
    ])
}

/// A zone (`ZONE`).
#[derive(Clone, Debug, PartialEq)]
pub struct Zone {
    /// `nickname` / `name`: its `CreateID` (the text is not kept).
    pub nickname: u32,
    /// The system's id when the zone was read.
    pub system: u32,
    /// `rotate`, through [`euler_matrix`]; identity by default.
    pub orientation: Matrix,
    /// `pos`.
    pub pos: Vector,
    /// `shape`.
    pub shape: ZoneShape,
    /// `property_flags` and the flags other keys add.
    pub property_flags: u32,
    /// `size`.
    pub size: Vector,
    /// `mesh`: the path the C++ hands to the engine (not loaded here).
    pub mesh: Vec<u8>,
    /// `edge_fraction`, clamped to 0..=1.
    pub edge_fraction: f32,
    /// `drag_modifier`, at least 1.
    pub drag_modifier: f32,
    /// `damage`.
    pub damage: f32,
    /// `interference`.
    pub interference: f32,
    /// `music`.
    pub music: u32,
    /// `spacedust`.
    pub spacedust: CacheString,
    /// `spacedust_maxparticles`.
    pub spacedust_max_particles: i32,
    /// `property_fog_color` divided by 255 (1, 1, 1).
    pub fog_color: Vector,
    /// `path_label`.
    pub path_label: bool,
    /// `faction` lines: `MakeId` of the faction and its weight.
    pub population: Vec<(u32, f32)>,
    /// `visit`, with 0x20 always set.
    pub visit: u8,
    /// `ids_name`.
    pub ids_name: i32,
    /// `ids_info`.
    pub ids_info: i32,
}

impl Zone {
    /// `ZONE()` for system `system`.
    #[must_use]
    pub fn new(system: u32) -> Self {
        Self {
            nickname: 0,
            system,
            orientation: Matrix::IDENTITY,
            pos: Vector::default(),
            shape: ZoneShape::Invalid,
            property_flags: 0,
            size: Vector::default(),
            mesh: Vec::new(),
            edge_fraction: 0.0,
            drag_modifier: 1.0,
            damage: 0.0,
            interference: 0.0,
            music: 0,
            spacedust: CacheString::EMPTY,
            spacedust_max_particles: 0,
            fog_color: Vector {
                x: 1.0,
                y: 1.0,
                z: 1.0,
            },
            path_label: false,
            population: Vec::new(),
            visit: 0x20,
            ids_name: 0,
            ids_info: 0,
        }
    }

    /// `ReadZoneValues`: the values of a `[Zone]` section.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) {
        while reader.read_value() {
            let is = |r: &IniReader, name: &[u8]| r.is_value(name);
            if is(reader, b"nickname") || is(reader, b"name") {
                self.nickname = create_id(&reader.value_string());
            } else if is(reader, b"pos") {
                self.pos = Vector {
                    x: value_f32(reader, 0),
                    y: value_f32(reader, 1),
                    z: value_f32(reader, 2),
                };
            } else if is(reader, b"rotate") {
                let angles = Vector {
                    x: value_f32(reader, 0),
                    y: value_f32(reader, 1),
                    z: value_f32(reader, 2),
                };
                self.orientation = euler_matrix(angles);
            } else if is(reader, b"shape") {
                self.shape = ZoneShape::parse(&reader.value_string_at(0), reports);
            } else if is(reader, b"size") {
                self.size.x = value_f32(reader, 0);
                self.size.y = if reader.is_value_empty(1) {
                    0.0
                } else {
                    value_f32(reader, 1)
                };
                self.size.z = if reader.is_value_empty(2) {
                    0.0
                } else {
                    value_f32(reader, 2)
                };
            } else if is(reader, b"mesh") {
                self.mesh = reader.value_string_at(0);
            } else if is(reader, b"edge_fraction") {
                let value = reader.value_float(0);
                self.edge_fraction = if value < 0.0 {
                    0.0
                } else if value < 1.0 {
                    value as f32
                } else {
                    1.0
                };
            } else if is(reader, b"property_flags") {
                self.property_flags |= u32::from_ne_bytes(reader.value_int(0).to_ne_bytes());
            } else if is(reader, b"property_fog_color") {
                let inv = 0.003_921_568_627_450_98;
                self.fog_color = Vector {
                    x: (reader.value_float(0) * inv) as f32,
                    y: (reader.value_float(1) * inv) as f32,
                    z: (reader.value_float(2) * inv) as f32,
                };
            } else if is(reader, b"drag_modifier") {
                let unrounded = reader.value_float(0);
                let value = unrounded as f32;
                if unrounded < 1.0 {
                    reports.push(
                        SOURCE,
                        0x9cd,
                        format!(
                            "*** ERROR: drag modifier should be >= 1.0  in zone (0x{:x}) in file ({})",
                            self.nickname,
                            lossy(reader.file_name())
                        ),
                    );
                }
                self.drag_modifier = if value < 1.0 { 1.0 } else { value };
                if value > 1.0 {
                    self.property_flags |= 0x8_0000;
                }
            } else if is(reader, b"damage") {
                let value = reader.value_float(0);
                self.damage = value as f32;
                if value > 0.0 {
                    self.property_flags |= 0x4_0000;
                }
            } else if is(reader, b"interference") {
                let value = reader.value_float(0);
                self.interference = value as f32;
                if value != 0.0 {
                    self.property_flags |= 0x10_0000;
                }
            } else if is(reader, b"music") {
                self.music = create_id(&reader.value_string());
                self.property_flags |= 0x40_0000;
            } else if is(reader, b"spacedust") {
                self.spacedust = strings.alloc(&reader.value_string(), false);
                self.property_flags |= 0x20_0000;
            } else if is(reader, b"spacedust_maxparticles") {
                self.spacedust_max_particles = reader.value_int(0);
            } else if is(reader, b"faction") {
                let faction = u32::from(make_id(&reader.value_string_at(0)));
                let weight = value_f32(reader, 1);
                self.population.push((faction, weight));
            } else if is(reader, b"path_label") {
                self.path_label = true;
            } else if is(reader, b"visit") {
                self.set_visit(reader.value_int(0).to_le_bytes()[0]);
            } else if is(reader, b"ids_name") {
                self.ids_name = reader.value_int(0);
            } else if is(reader, b"ids_info") {
                self.ids_info = reader.value_int(0);
            }
        }
    }

    /// `SetVisit`: 0x20 is always set.
    pub fn set_visit(&mut self, visit: u8) {
        self.visit = visit | 0x20;
    }

    /// `GetRadius`: the bounding radius of the shape.
    #[must_use]
    pub fn radius(&self) -> f32 {
        let (x, y, z) = (
            f64::from(self.size.x),
            f64::from(self.size.y),
            f64::from(self.size.z),
        );
        match self.shape {
            ZoneShape::Sphere => self.size.x,
            ZoneShape::Ellipsoid => {
                let largest = if self.size.y < self.size.x {
                    self.size.x
                } else {
                    self.size.y
                };
                if largest < self.size.z {
                    self.size.z
                } else {
                    largest
                }
            }
            ZoneShape::Box => (((x * x + y * y) + z * z).sqrt() * 0.5) as f32,
            ZoneShape::Cylinder => (x * x + (y * y) * 0.25).sqrt() as f32,
            ZoneShape::Ring => (x * x + (z * z) * 0.25).sqrt() as f32,
            ZoneShape::Invalid | ZoneShape::Mesh => 0.0,
        }
    }

    /// `HasPhysicalEffect`: drag below 1, interference or damage.
    #[must_use]
    pub fn has_physical_effect(&self) -> bool {
        self.drag_modifier < 1.0 || self.interference > 0.0 || self.damage > 0.0
    }
}
