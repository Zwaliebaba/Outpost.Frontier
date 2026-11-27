//! `layout.jsonl`'s records of the environment's files (P5.8): an
//! `asteroidfield` record per asteroid field file the client reads
//! (`OracleOnAsteroidFieldRead`, `oracle_mode.cpp`): the path, whether it
//! opened, `[Field]`'s values, the band, the exclusion bands, the
//! billboards and the dynamic asteroid sets, as the sections left them.

use super::json::OracleJson;
use crate::environment::asteroid_field::{AsteroidFieldFile, Band};

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
