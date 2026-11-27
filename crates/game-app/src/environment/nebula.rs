//! A nebula's file as the client reads it (`FNebulaEffect::Load`,
//! `cli_nebula.cpp`; work package P5.8): `[Exterior]`, `[Clouds]`,
//! `[Fog]`, `[TexturePanels]`, `[Lightning]`, `[BackgroundLightning]`,
//! `[DynamicLightning]`, `[NebulaLight]` and the names of `[Exclusion
//! Zones]`, over the defaults of the nebula's constructor and of `Load`.
//!
//! A few values neither sets (the exterior's `bit_radius` and `color`, the
//! fog's `near` and `color`) start at 0 here, where the reference leaves
//! the memory as it was (divergence D1700); every shipped nebula sets them.
//! The shapes found in the nebula's shape database, the zones, the exclusion
//! shells and the lights are the nebula's runtime.

use game_sim::data::{DataSource, open_ini};
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32_via_i64;

/// `ReadColor255`: three values times the float 1/255, stored as floats.
fn color(reader: &mut IniReader) -> [f32; 3] {
    let k = f64::from((1.0_f64 / 255.0) as f32);
    [0, 1, 2].map(|i| (reader.value_float(i) * k) as f32)
}

/// `(int)get_value_float(i)`: the float, then `ftol`.
fn int(reader: &mut IniReader, i: u32) -> i32 {
    trunc_to_i32_via_i64(f64::from(reader.value_float(i) as f32))
}

fn float(reader: &mut IniReader, i: u32) -> f32 {
    reader.value_float(i) as f32
}

/// `[Exterior]`.
#[derive(Clone, Debug, PartialEq)]
pub struct Exterior {
    /// `bit_radius` (D1700: 0 without it).
    pub bit_radius: f32,
    /// `color` over 255 (D1700: 0 without it).
    pub color: [f32; 3],
    /// `shape`, at most eight.
    pub shapes: Vec<Vec<u8>>,
    /// `shape_weights`, one per shape read before the key; 1.
    pub shape_weights: [f32; 8],
    /// `fill_shape`.
    pub fill_shape: Option<Vec<u8>>,
    /// `plane_slices`; 1 (`Load`'s).
    pub plane_slices: i32,
    /// `min_bits`; 4.
    pub min_bits: i32,
    /// `max_bits`; 8.
    pub max_bits: i32,
    /// `bit_radius_random_variation` v clamped to [0, 0.9999]: 1 - v and
    /// v + 1; both 1.
    pub bit_radius_range: [f32; 2],
    /// `equator_bias` as 1 - value; 0.
    pub equator_bias: f32,
    /// `move_bit_percent`; 0.75.
    pub move_bit_percent: f32,
    /// `opacity`; 0.75 (`Load`'s).
    pub opacity: f32,
}

impl Default for Exterior {
    fn default() -> Self {
        Exterior {
            bit_radius: 0.0,
            color: [0.0; 3],
            shapes: Vec::new(),
            shape_weights: [1.0; 8],
            fill_shape: None,
            plane_slices: 1,
            min_bits: 4,
            max_bits: 8,
            bit_radius_range: [1.0; 2],
            equator_bias: 0.0,
            move_bit_percent: 0.75,
            opacity: 0.75,
        }
    }
}

impl Exterior {
    /// `ParseExteriorSection`.
    fn parse(&mut self, reader: &mut IniReader) {
        while reader.read_value() {
            if reader.is_value(b"bit_radius") {
                self.bit_radius = float(reader, 0);
            } else if reader.is_value(b"color") {
                self.color = color(reader);
            } else if reader.is_value(b"shape") {
                if self.shapes.len() < 8 {
                    self.shapes.push(reader.value_string());
                }
            } else if reader.is_value(b"shape_weights") {
                for i in 0..self.shapes.len() {
                    self.shape_weights[i] = float(reader, u32::try_from(i).unwrap_or(0));
                }
            } else if reader.is_value(b"fill_shape") {
                self.fill_shape = Some(reader.value_string());
            } else if reader.is_value(b"plane_slices") {
                self.plane_slices = int(reader, 0);
            } else if reader.is_value(b"min_bits") {
                self.min_bits = int(reader, 0);
            } else if reader.is_value(b"max_bits") {
                self.max_bits = int(reader, 0);
            } else if reader.is_value(b"bit_radius_random_variation") {
                let mut v = float(reader, 0);
                if f64::from(v) > 0.9999 {
                    v = 0.9999;
                }
                if f64::from(v) < 0.0 {
                    v = 0.0;
                }
                self.bit_radius_range = [(1.0 - f64::from(v)) as f32, (f64::from(v) + 1.0) as f32];
            } else if reader.is_value(b"equator_bias") {
                self.equator_bias = (1.0 - f64::from(float(reader, 0))) as f32;
            } else if reader.is_value(b"move_bit_percent") {
                self.move_bit_percent = float(reader, 0);
            } else if reader.is_value(b"opacity") {
                self.opacity = float(reader, 0);
            }
        }
    }
}

/// `[Fog]`.
#[derive(Clone, Debug, PartialEq)]
pub struct Fog {
    /// `distance`; 10000 (`Load`'s).
    pub distance: f32,
    /// `near` (D1700: 0 without it).
    pub near: f32,
    /// `color` over 255, each kept only below 1 (D1700: 0 without it).
    pub color: [f32; 3],
}

impl Default for Fog {
    fn default() -> Self {
        Fog {
            distance: 10_000.0,
            near: 0.0,
            color: [0.0; 3],
        }
    }
}

/// `[Lightning]` (`LightningLoadIni`): the values the section gives.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Lightning {
    /// `gap`.
    pub gap: Option<f32>,
    /// `color` over 255.
    pub color: Option<[f32; 3]>,
    /// `size`.
    pub size: Option<f32>,
    /// `duration`.
    pub duration: Option<f32>,
}

/// `[BackgroundLightning]`.
#[derive(Clone, Debug, PartialEq)]
pub struct BackgroundLightning {
    /// `gap`; 10.
    pub gap: f32,
    /// `duration`; 0.5.
    pub duration: f32,
    /// `color` over 255; 0.25.
    pub color: [f32; 3],
    /// `intensity`; 1.
    pub intensity: f32,
}

impl Default for BackgroundLightning {
    fn default() -> Self {
        BackgroundLightning {
            gap: 10.0,
            duration: 0.5,
            color: [0.25; 3],
            intensity: 1.0,
        }
    }
}

/// `[DynamicLightning]`.
#[derive(Clone, Debug, PartialEq)]
pub struct DynamicLightning {
    /// `gap`; 3.
    pub gap: f32,
    /// `duration`; 0.5 (above 0, the nebula's ambient light flickers).
    pub duration: f32,
    /// `color` over 255; 1.
    pub color: [f32; 3],
    /// `ambient_intensity`; 0.25.
    pub ambient_intensity: f32,
    /// `intensity_increase`; 0.75.
    pub intensity_increase: f32,
}

impl Default for DynamicLightning {
    fn default() -> Self {
        DynamicLightning {
            gap: 3.0,
            duration: 0.5,
            color: [1.0; 3],
            ambient_intensity: 0.25,
            intensity_increase: 0.75,
        }
    }
}

/// `[NebulaLight]`.
#[derive(Clone, Debug, PartialEq)]
pub struct NebulaLight {
    /// `ambient`, each truncated to a byte; none without the key.
    pub ambient: Option<[u8; 3]>,
    /// `sun_burnthrough_intensity`; 0.5.
    pub sun_burnthrough_intensity: f32,
    /// `sun_burnthrough_scaler`; 1.75.
    pub sun_burnthrough_scaler: f32,
}

impl Default for NebulaLight {
    fn default() -> Self {
        NebulaLight {
            ambient: None,
            sun_burnthrough_intensity: 0.5,
            sun_burnthrough_scaler: 1.75,
        }
    }
}

/// `[Clouds]` (`LocalNebulaCloudsParseIni`): the values the parser sets,
/// over the defaults it stores first.
#[derive(Clone, Debug, PartialEq)]
pub struct Clouds {
    /// `puff_count`; 0.
    pub puff_count: i32,
    /// `puff_radius`; 50.
    pub puff_radius: f32,
    /// `puff_colora` / `puff_colorb` in the order given (at most two; the
    /// first fills both slots until a second comes).
    pub puff_colors: Vec<[f32; 3]>,
    /// `puff_shape`, at most eight.
    pub puff_shapes: Vec<Vec<u8>>,
    /// The shapes' weights, normalised to their sum; 1 before.
    pub puff_weights: [f32; 8],
    /// `puff_max_alpha`; 0.75.
    pub puff_max_alpha: f32,
    /// `puff_drift`; 0.
    pub puff_drift: f32,
    /// `lightning_shape`'s four names (each at most 31 bytes).
    pub lightning_shapes: Option<[Vec<u8>; 4]>,
    /// `lightning_color` over 255.
    pub lightning_color: Option<[f32; 3]>,
    /// `lightning_gap`.
    pub lightning_gap: Option<f32>,
    /// `lightning_duration`; 0.
    pub lightning_duration: f32,
    /// `lightning_size`; 1.
    pub lightning_size: f32,
    /// `llight_range`; 0.
    pub llight_range: f32,
    /// `llight_color` over 255.
    pub llight_color: Option<[f32; 3]>,
    /// `lightning_intensity`; 1.
    pub lightning_intensity: f32,
    /// `max_distance`; 1500.
    pub max_distance: f32,
    /// `near_fade_distance`, the smaller first ("near_fade_distances are
    /// flipped" otherwise); 50 and 100.
    pub near_fade: [f32; 2],
    /// `no_cloud_blend`; 0.
    pub no_cloud_blend: i32,
    /// `zwrite_clouds`; 0.
    pub zwrite_clouds: i32,
    /// `disable_clouds`; 0.
    pub disable_clouds: i32,
    /// `disable_wisps`.
    pub disable_wisps: Option<i32>,
    /// `disable_lightning_flash`; 0.
    pub disable_lightning_flash: i32,
    /// `disable_lightning_bolts`; 0.
    pub disable_lightning_bolts: i32,
    /// `disable_batcher`.
    pub disable_batcher: Option<i32>,
}

impl Default for Clouds {
    fn default() -> Self {
        Clouds {
            puff_count: 0,
            puff_radius: 50.0,
            puff_colors: Vec::new(),
            puff_shapes: Vec::new(),
            puff_weights: [1.0; 8],
            puff_max_alpha: 0.75,
            puff_drift: 0.0,
            lightning_shapes: None,
            lightning_color: None,
            lightning_gap: None,
            lightning_duration: 0.0,
            lightning_size: 1.0,
            llight_range: 0.0,
            llight_color: None,
            lightning_intensity: 1.0,
            max_distance: 1500.0,
            near_fade: [50.0, 100.0],
            no_cloud_blend: 0,
            zwrite_clouds: 0,
            disable_clouds: 0,
            disable_wisps: None,
            disable_lightning_flash: 0,
            disable_lightning_bolts: 0,
            disable_batcher: None,
        }
    }
}

impl Clouds {
    /// `LocalNebulaCloudsParseIni`.
    fn parse(reader: &mut IniReader) -> Clouds {
        let mut c = Clouds::default();
        while reader.read_value() {
            if reader.is_value(b"puff_count") {
                c.puff_count = int(reader, 0);
            } else if reader.is_value(b"puff_radius") {
                c.puff_radius = float(reader, 0);
            } else if reader.is_value(b"puff_colora") || reader.is_value(b"puff_colorb") {
                if c.puff_colors.len() < 2 {
                    c.puff_colors.push(color(reader));
                }
            } else if reader.is_value(b"puff_shape") {
                if c.puff_shapes.len() < 8 {
                    c.puff_shapes.push(reader.value_string());
                }
            } else if reader.is_value(b"puff_weights") {
                for i in 0..c.puff_shapes.len() {
                    c.puff_weights[i] = float(reader, u32::try_from(i).unwrap_or(0));
                }
            } else if reader.is_value(b"puff_max_alpha") {
                c.puff_max_alpha = float(reader, 0);
            } else if reader.is_value(b"puff_drift") {
                c.puff_drift = float(reader, 0);
            } else if reader.is_value(b"lightning_shape") {
                c.lightning_shapes = Some([0, 1, 2, 3].map(|i| {
                    let mut name = reader.value_string_at(i);
                    name.truncate(31);
                    name
                }));
            } else if reader.is_value(b"lightning_color") {
                c.lightning_color = Some(color(reader));
            } else if reader.is_value(b"lightning_gap") {
                c.lightning_gap = Some(float(reader, 0));
            } else if reader.is_value(b"lightning_duration") {
                c.lightning_duration = float(reader, 0);
            } else if reader.is_value(b"lightning_size") {
                c.lightning_size = float(reader, 0);
            } else if reader.is_value(b"llight_range") {
                c.llight_range = float(reader, 0);
            } else if reader.is_value(b"llight_color") {
                c.llight_color = Some(color(reader));
            } else if reader.is_value(b"lightning_intensity") {
                c.lightning_intensity = float(reader, 0);
            } else if reader.is_value(b"max_distance") {
                c.max_distance = float(reader, 0);
            } else if reader.is_value(b"near_fade_distance") {
                let (a, b) = (float(reader, 0), float(reader, 1));
                c.near_fade = if b < a { [b, a] } else { [a, b] };
            } else if reader.is_value(b"no_cloud_blend") {
                c.no_cloud_blend = int(reader, 0);
            } else if reader.is_value(b"zwrite_clouds") {
                c.zwrite_clouds = int(reader, 0);
            } else if reader.is_value(b"disable_clouds") {
                c.disable_clouds = int(reader, 0);
            } else if reader.is_value(b"disable_wisps") {
                c.disable_wisps = Some(int(reader, 0));
            } else if reader.is_value(b"disable_lightning_flash") {
                c.disable_lightning_flash = int(reader, 0);
            } else if reader.is_value(b"disable_lightning_bolts") {
                c.disable_lightning_bolts = int(reader, 0);
            } else if reader.is_value(b"disable_batcher") {
                c.disable_batcher = Some(int(reader, 0));
            }
        }
        // The weights over their sum (a double), each stored as a float.
        let n = c.puff_shapes.len();
        if n > 0 {
            let sum: f64 = c.puff_weights[..n].iter().map(|&w| f64::from(w)).sum();
            // `1.0f / Sum` is a double division; the inverse is not stored.
            let inv = 1.0 / sum;
            for w in &mut c.puff_weights[..n] {
                *w = (inv * f64::from(*w)) as f32;
            }
        }
        c
    }
}

/// A nebula's file.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NebulaFile {
    /// The file opened ("unable to open nebula file" otherwise).
    pub opened: bool,
    /// `[Exterior]`.
    pub exterior: Exterior,
    /// Each `[Clouds]`.
    pub clouds: Vec<Clouds>,
    /// `[Fog]` (`[ObjectFog]` is skipped).
    pub fog: Fog,
    /// Every `[TexturePanels]` `file`, in order.
    pub texture_panels: Vec<Vec<u8>>,
    /// `[Lightning]`, once one was read.
    pub lightning: Option<Lightning>,
    /// `[BackgroundLightning]`.
    pub background_lightning: BackgroundLightning,
    /// `[DynamicLightning]`.
    pub dynamic_lightning: DynamicLightning,
    /// `[NebulaLight]`.
    pub light: NebulaLight,
    /// The `exclusion` names of each `[Exclusion Zones]` section, in order.
    pub exclusions: Vec<Vec<u8>>,
}

impl NebulaFile {
    /// `FNebulaEffect::Load`'s reading of `path` (relative to the data) on
    /// a fresh nebula.
    #[must_use]
    pub fn load(source: &dyn DataSource, path: &[u8]) -> NebulaFile {
        let mut out = NebulaFile::default();
        let Some(mut reader) = open_ini(source, path) else {
            return out;
        };
        out.opened = true;
        out.read(&mut reader);
        out
    }

    fn read(&mut self, reader: &mut IniReader) {
        while reader.read_header() {
            if reader.is_header(b"Exterior") {
                self.exterior.parse(reader);
            } else if reader.is_header(b"Clouds") {
                self.clouds.push(Clouds::parse(reader));
            } else if reader.is_header(b"Fog") {
                self.read_fog(reader);
            } else if reader.is_header(b"TexturePanels") {
                while reader.read_value() {
                    if reader.is_value(b"file") {
                        self.texture_panels.push(reader.value_string());
                    }
                }
            } else if reader.is_header(b"Lightning") {
                let l = self.lightning.get_or_insert_with(Lightning::default);
                while reader.read_value() {
                    if reader.is_value(b"gap") {
                        l.gap = Some(float(reader, 0));
                    } else if reader.is_value(b"color") {
                        l.color = Some(color(reader));
                    } else if reader.is_value(b"size") {
                        l.size = Some(float(reader, 0));
                    } else if reader.is_value(b"duration") {
                        l.duration = Some(float(reader, 0));
                    }
                }
            } else if reader.is_header(b"BackgroundLightning") {
                let b = &mut self.background_lightning;
                while reader.read_value() {
                    if reader.is_value(b"gap") {
                        b.gap = float(reader, 0);
                    } else if reader.is_value(b"duration") {
                        b.duration = float(reader, 0);
                    } else if reader.is_value(b"color") {
                        b.color = color(reader);
                    } else if reader.is_value(b"intensity") {
                        b.intensity = float(reader, 0);
                    }
                }
            } else if reader.is_header(b"DynamicLightning") {
                let d = &mut self.dynamic_lightning;
                while reader.read_value() {
                    if reader.is_value(b"gap") {
                        d.gap = float(reader, 0);
                    } else if reader.is_value(b"duration") {
                        d.duration = float(reader, 0);
                    } else if reader.is_value(b"color") {
                        d.color = color(reader);
                    } else if reader.is_value(b"ambient_intensity") {
                        d.ambient_intensity = float(reader, 0);
                    } else if reader.is_value(b"intensity_increase") {
                        d.intensity_increase = float(reader, 0);
                    }
                }
            } else if reader.is_header(b"NebulaLight") {
                let n = &mut self.light;
                while reader.read_value() {
                    if reader.is_value(b"ambient") {
                        n.ambient = Some([0, 1, 2].map(|i| int(reader, i).to_le_bytes()[0]));
                    } else if reader.is_value(b"sun_burnthrough_intensity") {
                        n.sun_burnthrough_intensity = float(reader, 0);
                    } else if reader.is_value(b"sun_burnthrough_scaler") {
                        n.sun_burnthrough_scaler = float(reader, 0);
                    }
                }
            } else if reader.is_header(b"Exclusion Zones") {
                while reader.read_value() {
                    if reader.is_value(b"exclusion") {
                        self.exclusions.push(reader.value_string());
                    }
                }
            }
        }
    }

    /// `ParseFogSection`.
    fn read_fog(&mut self, reader: &mut IniReader) {
        let f = &mut self.fog;
        while reader.read_value() {
            if reader.is_value(b"distance") {
                f.distance = float(reader, 0);
            } else if reader.is_value(b"near") {
                f.near = float(reader, 0);
            } else if reader.is_value(b"color") {
                // Each kept only below 1 (a NaN or anything else is 1).
                f.color = color(reader).map(|c| if c < 1.0 { c } else { 1.0 });
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(text: &str) -> NebulaFile {
        let mut reader = IniReader::new();
        assert!(reader.open(b"nebula.ini", text.as_bytes().to_vec()));
        let mut out = NebulaFile {
            opened: true,
            ..NebulaFile::default()
        };
        out.read(&mut reader);
        out
    }

    #[test]
    fn a_nebula_reads_its_sections_over_the_defaults() {
        let n = read(
            "[Exterior]\nshape = a\nshape = b\nshape_weights = 1, 3\nbit_radius_random_variation = 2\n\
             plane_slices = 6\n[Fog]\ncolor = 255, 510, 0\nnear = 5\n\
             [Clouds]\npuff_shape = x\npuff_shape = y\npuff_weights = 1, 3\npuff_colorb = 255, 0, 0\n\
             near_fade_distance = 300, 200\n[NebulaLight]\nambient = 300, 1, 2\n\
             [Exclusion Zones]\nexclusion = z1\nshell_scalar = 2\nexclusion = z2\n",
        );
        let e = &n.exterior;
        assert_eq!(e.shapes.len(), 2);
        assert_eq!(&e.shape_weights[..3], [1.0, 3.0, 1.0]);
        // The clamp is the float 0.9999f, so 1 - v is not 0.0001.
        let v = f64::from(0.9999_f32);
        assert_eq!(e.bit_radius_range, [(1.0 - v) as f32, (v + 1.0) as f32]);
        assert_eq!((e.plane_slices, e.min_bits, e.opacity), (6, 4, 0.75));
        assert_eq!(n.fog.color, [1.0, 1.0, 0.0]);
        assert_eq!((n.fog.near, n.fog.distance), (5.0, 10_000.0));
        let c = &n.clouds[0];
        assert_eq!(&c.puff_weights[..2], [0.25, 0.75]);
        assert_eq!(c.puff_colors, [[1.0, 0.0, 0.0]]);
        assert_eq!(c.near_fade, [200.0, 300.0]);
        assert_eq!(n.light.ambient, Some([44, 1, 2]));
        assert_eq!(n.exclusions, [b"z1".to_vec(), b"z2".to_vec()]);
        assert_eq!(n.background_lightning.gap, 10.0);
    }
}
