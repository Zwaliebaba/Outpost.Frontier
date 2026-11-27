//! An asteroid field's file as the client reads it (`CliAsteroid::load`,
//! `CliAsteroidLoadFieldIni`, `cli_asteroid.cpp`; work package P5.8): the
//! texture panels, `[Field]`, the field's `[Band]`, the `[ExclusionBand]`s,
//! `[AsteroidBillboards]` and the `[DynamicAsteroids]`, each with the
//! defaults of the client field's constructor for what the file leaves out.
//!
//! What the load does with the values afterwards is the field's runtime:
//! the shapes found in the field's shape database, the zones looked up in
//! the active system, the tints applied to the bands and billboards, the
//! common field (`game-sim::data::asteroid_field`) loaded from the same
//! reader, and the meshes. The names are kept here as the file gives them.

use game_sim::data::{DataSource, open_ini};
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32_via_i64;

/// `1.0f / 255.0f`, the colour keys' scale.
fn inv_255() -> f64 {
    f64::from((1.0_f64 / 255.0) as f32)
}

/// `FloatToIntTrunc`: the value as the `float` parameter holds it, then
/// `ftol`.
fn trunc(value: f64) -> i32 {
    trunc_to_i32_via_i64(f64::from(value as f32))
}

/// A colour key's three values over 255, stored as floats.
fn color(reader: &mut IniReader) -> [f32; 3] {
    [0, 1, 2].map(|i| (reader.value_float(i) * inv_255()) as f32)
}

fn three(reader: &mut IniReader) -> [f32; 3] {
    [0, 1, 2].map(|i| reader.value_float(i) as f32)
}

/// `[Field]` and what `CliAsteroidLoadFieldIni` resets before it.
#[derive(Clone, Debug, PartialEq)]
pub struct FieldParams {
    /// `cube_size`; 250 when the file has none (the load's reset).
    pub cube_size: i32,
    /// `fill_dist`; 1200 (the constructor's).
    pub fill_dist: i32,
    /// `LOD`, which the mesh merge takes; 0.
    pub lod: i32,
    /// `diffuse_color` (or `tint_field`) over 255; 1.
    pub diffuse: [f32; 3],
    /// `ambient_color` (or `tint_field`) over 255; 1.
    pub ambient: [f32; 3],
    /// `ambient_increase`, each truncated to a byte; 0.
    pub ambient_increase: [u8; 3],
    /// `empty_cube_frequency`; 0.
    pub empty_cube_frequency: f32,
    /// `max_alpha`; 1.
    pub max_alpha: f32,
    /// `test_asteroid_placement` is present.
    pub test_asteroid_placement: bool,
    /// `contains_fog_zone` is present.
    pub contains_fog_zone: bool,
}

impl Default for FieldParams {
    fn default() -> Self {
        FieldParams {
            cube_size: 250,
            fill_dist: 1200,
            lod: 0,
            diffuse: [1.0; 3],
            ambient: [1.0; 3],
            ambient_increase: [0; 3],
            empty_cube_frequency: 0.0,
            max_alpha: 1.0,
            test_asteroid_placement: false,
            contains_fog_zone: false,
        }
    }
}

/// A band's values (`InitBandBlob`, `BandParseIni`): the field's own band
/// or an exclusion band.
#[derive(Clone, Debug, PartialEq)]
pub struct Band {
    /// `shape`, a name of the field's shape database.
    pub shape: Option<Vec<u8>>,
    /// `texture_aspect` times 1.15; 1.
    pub texture_aspect: f32,
    /// `height`; 1000.
    pub height: f32,
    /// `offset_percent` (rather than `offset_dist`) was the last offset key.
    pub offset_percent: bool,
    /// The offset; 0.
    pub offset: f32,
    /// `color_shift`; 1.
    pub color_shift: [f32; 3],
    /// `fade`: four distances; 1500, 2500, 100000, 130000.
    pub fade: [f32; 4],
    /// `vert_increase`, at least 1; 1.
    pub vert_increase: u32,
    /// `render_parts`, at least 1; 1 once a section is read.
    pub render_parts: u32,
    /// `cull_mode`; 0.
    pub cull_mode: u32,
    /// `ambient_intensity`; 1 once a section is read.
    pub ambient_intensity: f32,
    /// An exclusion band's `zone` values, in order (the last one found in
    /// the system is the band's zone).
    pub zones: Vec<Vec<u8>>,
}

impl Default for Band {
    /// `InitBandBlob`.
    fn default() -> Self {
        Band {
            shape: None,
            texture_aspect: 1.0,
            height: 1000.0,
            offset_percent: false,
            offset: 0.0,
            color_shift: [1.0; 3],
            fade: [1500.0, 2500.0, 100_000.0, 130_000.0],
            vert_increase: 1,
            render_parts: 0,
            cull_mode: 0,
            ambient_intensity: 1.0,
            zones: Vec::new(),
        }
    }
}

impl Band {
    /// `BandParseIni`: the section's values over what the band holds.
    fn parse(&mut self, reader: &mut IniReader) {
        self.ambient_intensity = 1.0;
        self.render_parts = 1;
        while reader.read_value() {
            if reader.is_value(b"shape") {
                self.shape = Some(reader.value_string());
            } else if reader.is_value(b"texture_aspect") {
                self.texture_aspect = (f64::from(reader.value_float(0) as f32) * 1.15) as f32;
            } else if reader.is_value(b"height") {
                self.height = reader.value_float(0) as f32;
            } else if reader.is_value(b"offset_dist") {
                self.offset_percent = false;
                self.offset = reader.value_float(0) as f32;
            } else if reader.is_value(b"offset_percent") {
                self.offset_percent = true;
                self.offset = reader.value_float(0) as f32;
            } else if reader.is_value(b"color_shift") {
                self.color_shift = three(reader);
            } else if reader.is_value(b"fade") {
                self.fade = [0, 1, 2, 3].map(|i| reader.value_float(i) as f32);
            } else if reader.is_value(b"vert_increase") {
                self.vert_increase = trunc(reader.value_float(0)).max(1).cast_unsigned();
            } else if reader.is_value(b"render_parts") {
                self.render_parts = trunc(reader.value_float(0)).max(1).cast_unsigned();
            } else if reader.is_value(b"cull_mode") {
                self.cull_mode = trunc(reader.value_float(0)).cast_unsigned();
            } else if reader.is_value(b"ambient_intensity") {
                self.ambient_intensity = reader.value_float(0) as f32;
            }
        }
    }
}

/// `[AsteroidBillboards]` (the field's billboard block).
#[derive(Clone, Debug, PartialEq)]
pub struct Billboards {
    /// `shape`.
    pub shape: Option<Vec<u8>>,
    /// `count`; 100.
    pub count: i32,
    /// `size`, the smaller first; 500 and 100 (the constructor's, in that
    /// order).
    pub size: [f32; 2],
    /// `start_dist`; 1500.
    pub start_dist: f32,
    /// `fade_dist_percent`, clamped to [0, 1]; 0.3.
    pub fade_dist_percent: f32,
    /// `ambient_intensity`; 1.
    pub ambient_intensity: f32,
    /// `color_shift`; 1.
    pub color_shift: [f32; 3],
}

impl Default for Billboards {
    fn default() -> Self {
        Billboards {
            shape: None,
            count: 100,
            size: [500.0, 100.0],
            start_dist: 1500.0,
            fade_dist_percent: 0.3,
            ambient_intensity: 1.0,
            color_shift: [1.0; 3],
        }
    }
}

impl Billboards {
    /// `BillboardParseIni`.
    fn parse(&mut self, reader: &mut IniReader) {
        self.fade_dist_percent = 0.3;
        while reader.read_value() {
            if reader.is_value(b"shape") {
                self.shape = Some(reader.value_string());
            } else if reader.is_value(b"count") {
                self.count = trunc(reader.value_float(0));
            } else if reader.is_value(b"size") {
                let a = reader.value_float(0) as f32;
                let b = reader.value_float(1) as f32;
                self.size = if b < a { [b, a] } else { [a, b] };
            } else if reader.is_value(b"start_dist") {
                self.start_dist = reader.value_float(0) as f32;
            } else if reader.is_value(b"fade_dist_percent") {
                let v = reader.value_float(0) as f32;
                // A NaN passes, as through the reference's two tests.
                self.fade_dist_percent = v.clamp(0.0, 1.0);
            } else if reader.is_value(b"ambient_intensity") {
                self.ambient_intensity = reader.value_float(0) as f32;
            } else if reader.is_value(b"color_shift") {
                self.color_shift = three(reader);
            }
        }
    }
}

/// `[DynamicAsteroids]` (`CliDynamicAsteroid`).
#[derive(Clone, Debug, PartialEq)]
pub struct DynamicAsteroids {
    /// `asteroid`, the dynamic asteroid archetype's nickname.
    pub asteroid: Option<Vec<u8>>,
    /// `count`; 10.
    pub count: i32,
    /// `placement_radius`; 150.
    pub placement_radius: f32,
    /// The radius times 0.7; 75.
    pub placement_radius_inner: f32,
    /// `max_velocity`; 50.
    pub max_velocity: f32,
    /// `max_angular_velocity`; 5.
    pub max_angular_velocity: f32,
    /// `color_shift`; 1.
    pub color_shift: [f32; 3],
    /// The `zone` values, in order (the last one found in the system is
    /// the manager's zone; none found, the field's).
    pub zones: Vec<Vec<u8>>,
}

impl Default for DynamicAsteroids {
    /// `CliDynamicAsteroidConstruct`.
    fn default() -> Self {
        DynamicAsteroids {
            asteroid: None,
            count: 10,
            placement_radius: 150.0,
            placement_radius_inner: 75.0,
            max_velocity: 50.0,
            max_angular_velocity: 5.0,
            color_shift: [1.0; 3],
            zones: Vec::new(),
        }
    }
}

impl DynamicAsteroids {
    /// `CliDynamicAsteroidParseIni`'s values.
    fn parse(&mut self, reader: &mut IniReader) {
        while reader.read_value() {
            if reader.is_value(b"asteroid") {
                self.asteroid = Some(reader.value_string_at(0));
            } else if reader.is_value(b"count") {
                self.count = trunc(reader.value_float(0));
            } else if reader.is_value(b"placement_radius") {
                let radius = reader.value_float(0) as f32;
                self.placement_radius = radius;
                self.placement_radius_inner = (f64::from(radius) * 0.7) as f32;
            } else if reader.is_value(b"max_velocity") {
                self.max_velocity = reader.value_float(0) as f32;
            } else if reader.is_value(b"max_angular_velocity") {
                self.max_angular_velocity = reader.value_float(0) as f32;
            } else if reader.is_value(b"color_shift") {
                self.color_shift = three(reader);
            }
        }
    }
}

/// The `zone` values of the section `reader` stands at.
fn zones(reader: &mut IniReader) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"zone") {
            out.push(reader.value_string());
        }
    }
    out
}

/// An asteroid field's file.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AsteroidFieldFile {
    /// The file opened ("asteroid field ini file (...) could not be opened"
    /// otherwise, and nothing else is read).
    pub opened: bool,
    /// Every `[TexturePanels]` `file`, in order.
    pub texture_panels: Vec<Vec<u8>>,
    /// `[Field]`.
    pub field: FieldParams,
    /// The field's own band, once a `[Band]` was read (each further one
    /// reads over it).
    pub band: Option<Band>,
    /// Each `[ExclusionBand]`.
    pub exclusion_bands: Vec<Band>,
    /// `[AsteroidBillboards]`, once one was read (each further one reads
    /// over it).
    pub billboards: Option<Billboards>,
    /// Each `[DynamicAsteroids]`.
    pub dynamic: Vec<DynamicAsteroids>,
}

impl AsteroidFieldFile {
    /// `CliAsteroidLoadFieldIni`'s reading of `path` (relative to the
    /// data) on a fresh field.
    #[must_use]
    pub fn load(source: &dyn DataSource, path: &[u8]) -> AsteroidFieldFile {
        let mut out = AsteroidFieldFile::default();
        let Some(mut reader) = open_ini(source, path) else {
            return out;
        };
        out.opened = true;
        out.read(&mut reader);
        out
    }

    fn read(&mut self, reader: &mut IniReader) {
        while reader.read_header() {
            if reader.is_header(b"TexturePanels") {
                while reader.read_value() {
                    if reader.is_value(b"file") {
                        self.texture_panels.push(reader.value_string());
                    }
                }
            } else if reader.is_header(b"Field") {
                self.read_field(reader);
            } else if reader.is_header(b"Band") {
                self.band.get_or_insert_with(Band::default).parse(reader);
            } else if reader.is_header(b"ExclusionBand") {
                let state = reader.state();
                let mut band = Band::default();
                band.parse(reader);
                reader.set_state(&state);
                band.zones = zones(reader);
                self.exclusion_bands.push(band);
            } else if reader.is_header(b"AsteroidBillboards") {
                self.billboards
                    .get_or_insert_with(Billboards::default)
                    .parse(reader);
            } else if reader.is_header(b"DynamicAsteroids") {
                let state = reader.state();
                let mut dynamic = DynamicAsteroids::default();
                dynamic.parse(reader);
                reader.set_state(&state);
                dynamic.zones = zones(reader);
                self.dynamic.push(dynamic);
            }
        }
    }

    /// `ParseFieldSection`.
    fn read_field(&mut self, reader: &mut IniReader) {
        let f = &mut self.field;
        while reader.read_value() {
            if reader.is_value(b"cube_size") {
                f.cube_size = trunc(reader.value_float(0));
            } else if reader.is_value(b"fill_dist") {
                f.fill_dist = trunc(reader.value_float(0));
            } else if reader.is_value(b"LOD") {
                f.lod = trunc(reader.value_float(0));
            } else if reader.is_value(b"tint_field") {
                let c = color(reader);
                f.diffuse = c;
                f.ambient = c;
            } else if reader.is_value(b"diffuse_color") {
                f.diffuse = color(reader);
            } else if reader.is_value(b"ambient_color") {
                f.ambient = color(reader);
            } else if reader.is_value(b"ambient_increase") {
                // The low byte of the truncated value.
                f.ambient_increase =
                    [0, 1, 2].map(|i| trunc(reader.value_float(i)).to_le_bytes()[0]);
            } else if reader.is_value(b"empty_cube_frequency") {
                f.empty_cube_frequency = reader.value_float(0) as f32;
            } else if reader.is_value(b"max_alpha") {
                f.max_alpha = reader.value_float(0) as f32;
            } else if reader.is_value(b"test_asteroid_placement") {
                f.test_asteroid_placement = true;
            } else if reader.is_value(b"contains_fog_zone") {
                f.contains_fog_zone = true;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(text: &str) -> AsteroidFieldFile {
        let mut reader = IniReader::new();
        assert!(reader.open(b"field.ini", text.as_bytes().to_vec()));
        let mut out = AsteroidFieldFile {
            opened: true,
            ..AsteroidFieldFile::default()
        };
        out.read(&mut reader);
        out
    }

    #[test]
    fn a_field_reads_its_sections_over_the_defaults() {
        let f = read(
            "[TexturePanels]\nfile = a.txm\n[Field]\ncube_size = 600\ntint_field = 255, 0, 51\n\
             ambient_increase = 300, 2, 3\n[Band]\nshape = rock\nrender_parts = 0\nfade = 1, 2, 3, 4\n\
             texture_aspect = 2\n[ExclusionBand]\nzone = z1\noffset_percent = 0.5\nzone = z2\n\
             [AsteroidBillboards]\nsize = 50, 10\nfade_dist_percent = 4\n\
             [DynamicAsteroids]\nasteroid = dyn, x\nplacement_radius = 100\nzone = z3\n",
        );
        assert_eq!(f.texture_panels, [b"a.txm".to_vec()]);
        assert_eq!(f.field.cube_size, 600);
        assert_eq!(f.field.fill_dist, 1200);
        // 51 times the float 1/255 in double, then stored: not 0.2f.
        let fifth = (51.0 * f64::from((1.0_f64 / 255.0) as f32)) as f32;
        assert_eq!(f.field.diffuse, [1.0, 0.0, fifth]);
        assert_ne!(fifth, 0.2);
        assert_eq!(f.field.ambient, f.field.diffuse);
        assert_eq!(f.field.ambient_increase, [44, 2, 3]);
        let band = f.band.unwrap();
        assert_eq!(
            (band.shape.as_deref(), band.render_parts),
            (Some(&b"rock"[..]), 1)
        );
        assert_eq!(band.fade, [1.0, 2.0, 3.0, 4.0]);
        assert_eq!(band.texture_aspect, 2.3);
        let ex = &f.exclusion_bands[0];
        assert_eq!(ex.zones, [b"z1".to_vec(), b"z2".to_vec()]);
        assert!(ex.offset_percent && ex.offset == 0.5);
        let bb = f.billboards.unwrap();
        assert_eq!(
            (bb.size, bb.fade_dist_percent, bb.count),
            ([10.0, 50.0], 1.0, 100)
        );
        let d = &f.dynamic[0];
        assert_eq!(d.asteroid.as_deref(), Some(&b"dyn"[..]));
        assert_eq!(
            (d.placement_radius, d.placement_radius_inner),
            (100.0, 70.0)
        );
        assert_eq!(d.zones, [b"z3".to_vec()]);
        assert_eq!(d.count, 10);
    }
}
