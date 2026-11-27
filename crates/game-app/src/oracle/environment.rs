//! `layout.jsonl`'s records of the environment's files (P5.8): an
//! `asteroidfield` record per asteroid field file the client reads
//! (`OracleOnAsteroidFieldRead`, `oracle_mode.cpp`): the path, whether it
//! opened, `[Field]`'s values, the band, the exclusion bands, the
//! billboards and the dynamic asteroid sets, as the sections left them;
//! and a `nebula` record per nebula file (`OracleOnNebulaRead`): the values
//! the constructor, `Load` or the section readers set.

use super::json::OracleJson;
use crate::environment::asteroid_field::{AsteroidFieldFile, Band};
use crate::environment::nebula::NebulaFile;

fn floats(j: &mut OracleJson, name: &str, values: &[f32]) {
    j.begin_array(name);
    for &v in values {
        j.item_float(v);
    }
    j.end_array();
}

fn band(j: &mut OracleJson, b: &Band) {
    j.float("texture_aspect", b.texture_aspect)
        .float("height", b.height)
        .bool("offset_percent", b.offset_percent)
        .float("offset", b.offset);
    floats(j, "color_shift", &b.color_shift);
    floats(j, "fade", &b.fade);
    j.uint("vert_increase", b.vert_increase)
        .uint("render_parts", b.render_parts)
        .uint("cull_mode", b.cull_mode)
        .float("ambient_intensity", b.ambient_intensity)
        .end_object();
}

/// The `asteroidfield` record of `file`, read from `path`, at `frame`. A
/// file that did not open has the defaults of a fresh field.
#[must_use]
pub fn asteroid_field_line(frame: u32, path: &[u8], file: &AsteroidFieldFile) -> String {
    let f = &file.field;
    let mut j = OracleJson::new();
    j.uint("frame", frame)
        .str("rec", "asteroidfield")
        .str_bytes("path", path)
        .bool("opened", file.opened)
        .int("cube_size", f.cube_size)
        .int("fill_dist", f.fill_dist)
        .int("lod", f.lod);
    floats(&mut j, "diffuse", &f.diffuse);
    floats(&mut j, "ambient", &f.ambient);
    j.begin_array("ambient_increase");
    for &v in &f.ambient_increase {
        j.item_uint(u32::from(v));
    }
    j.end_array()
        .float("empty_cube_frequency", f.empty_cube_frequency)
        .float("max_alpha", f.max_alpha)
        .bool("test_placement", f.test_asteroid_placement)
        .bool("fog_zone", f.contains_fog_zone);
    match &file.band {
        Some(b) => {
            j.begin_object("band");
            band(&mut j, b);
        }
        None => {
            j.null("band");
        }
    }
    j.begin_array("exclusion_bands");
    for b in &file.exclusion_bands {
        j.begin_object_item();
        band(&mut j, b);
    }
    j.end_array();
    match &file.billboards {
        Some(b) => {
            j.begin_object("billboards").int("count", b.count);
            floats(&mut j, "size", &b.size);
            j.float("start_dist", b.start_dist)
                .float("fade_dist_percent", b.fade_dist_percent)
                .float("ambient_intensity", b.ambient_intensity);
            floats(&mut j, "color_shift", &b.color_shift);
            j.end_object();
        }
        None => {
            j.null("billboards");
        }
    }
    j.begin_array("dynamic");
    for d in &file.dynamic {
        j.begin_object_item()
            .int("count", d.count)
            .float("placement_radius", d.placement_radius)
            .float("placement_radius_inner", d.placement_radius_inner)
            .float("max_velocity", d.max_velocity)
            .float("max_angular_velocity", d.max_angular_velocity);
        floats(&mut j, "color_shift", &d.color_shift);
        j.end_object();
    }
    j.end_array();
    j.finish()
}

fn len(n: usize) -> i32 {
    i32::try_from(n).unwrap_or(i32::MAX)
}

/// The `nebula` record of `file`, read from `path`, at `frame`.
#[must_use]
pub fn nebula_line(frame: u32, path: &[u8], file: &NebulaFile) -> String {
    let mut j = OracleJson::new();
    j.uint("frame", frame)
        .str("rec", "nebula")
        .str_bytes("path", path)
        .bool("opened", file.opened);
    let e = &file.exterior;
    j.begin_object("exterior").float("bit_radius", e.bit_radius);
    floats(&mut j, "color", &e.color);
    j.int("shapes", len(e.shapes.len()));
    floats(&mut j, "shape_weights", &e.shape_weights);
    j.int("plane_slices", e.plane_slices)
        .int("min_bits", e.min_bits)
        .int("max_bits", e.max_bits);
    floats(&mut j, "bit_radius_range", &e.bit_radius_range);
    j.float("equator_bias", e.equator_bias)
        .float("move_bit_percent", e.move_bit_percent)
        .float("opacity", e.opacity)
        .end_object();
    let f = &file.fog;
    j.begin_object("fog")
        .float("distance", f.distance)
        .float("near", f.near);
    floats(&mut j, "color", &f.color);
    j.end_object();
    let b = &file.background_lightning;
    j.begin_object("background_lightning")
        .float("gap", b.gap)
        .float("duration", b.duration);
    floats(&mut j, "color", &b.color);
    j.float("intensity", b.intensity).end_object();
    let d = &file.dynamic_lightning;
    j.begin_object("dynamic_lightning")
        .float("gap", d.gap)
        .float("duration", d.duration);
    floats(&mut j, "color", &d.color);
    j.float("ambient_intensity", d.ambient_intensity)
        .float("intensity_increase", d.intensity_increase)
        .end_object();
    let l = &file.light;
    j.begin_object("light");
    match l.ambient {
        Some(a) => {
            j.begin_array("ambient");
            for v in a {
                j.item_uint(u32::from(v));
            }
            j.end_array();
        }
        None => {
            j.null("ambient");
        }
    }
    j.float("sun_burnthrough_intensity", l.sun_burnthrough_intensity)
        .float("sun_burnthrough_scaler", l.sun_burnthrough_scaler)
        .end_object();
    j.begin_array("clouds");
    for c in &file.clouds {
        j.begin_object_item()
            .int("puff_count", c.puff_count)
            .float("puff_radius", c.puff_radius)
            .int("puff_shapes", len(c.puff_shapes.len()));
        floats(&mut j, "puff_weights", &c.puff_weights);
        j.float("puff_max_alpha", c.puff_max_alpha)
            .float("puff_drift", c.puff_drift)
            .float("lightning_duration", c.lightning_duration)
            .float("lightning_size", c.lightning_size)
            .float("lightning_intensity", c.lightning_intensity)
            .float("llight_range", c.llight_range)
            .float("max_distance", c.max_distance);
        floats(&mut j, "near_fade", &c.near_fade);
        j.int("no_cloud_blend", c.no_cloud_blend)
            .int("zwrite_clouds", c.zwrite_clouds)
            .int("disable_clouds", c.disable_clouds)
            .int("disable_lightning_flash", c.disable_lightning_flash)
            .int("disable_lightning_bolts", c.disable_lightning_bolts)
            .end_object();
    }
    j.end_array();
    j.finish()
}
