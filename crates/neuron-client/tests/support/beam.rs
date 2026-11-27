//! The lines of `CppSource/Oracle/oracle_beam.cpp`: each `beam` case of
//! `beam.list` built by `render::effects::beam::build_geometry` into a reset
//! batch, the beam watching nothing, the `particles` option on and the
//! cockpit camera not current (the process's stand-ins).

use neuron_client::render::effects::batch::EffectVertexBatch;
use neuron_client::render::effects::beam::{BeamFrame, BeamShape, BeamState, build_geometry};
use neuron_core::math::Transform;

fn f32_hex(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

fn floats<const N: usize>(text: &str) -> [f32; N] {
    let values: Vec<f32> = text
        .split(',')
        .map(|w| f32::from_bits(u32::from_str_radix(w, 16).unwrap_or_else(|e| panic!("{w}: {e}"))))
        .collect();
    values
        .try_into()
        .unwrap_or_else(|v: Vec<f32>| panic!("{N} floats expected, {} read", v.len()))
}

fn rgb(a: &[f32; 28], at: usize) -> [f32; 3] {
    [a[at], a[at + 1], a[at + 2]]
}

/// The archetype's 28 floats from +0x10 as the shape, without
/// `sec_core_width` (+0x70) and +0x7c, which the geometry does not read.
fn shape(mode: i32, head: i32, trail: i32, a: &[f32; 28]) -> BeamShape {
    BeamShape {
        segment_mode: mode,
        head_texture: head,
        trail_texture: trail,
        tip_color: rgb(a, 0),
        tail_color: rgb(a, 3),
        core_color: rgb(a, 6),
        sec_core_color: rgb(a, 9),
        outer_color: rgb(a, 12),
        sec_outer_color: rgb(a, 15),
        head_brightness: a[18],
        trail_brightness: a[19],
        tip_length: a[20],
        tail_length: a[21],
        core_length: a[22],
        core_width: a[23],
        head_width: a[25],
        flash_size: a[26],
    }
}

pub fn lines(input: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    let mut batch = EffectVertexBatch::default();
    let frame = BeamFrame {
        particles: true,
        first_person: false,
    };
    for (case, line) in input.iter().enumerate() {
        let w: Vec<&str> = line.split(' ').collect();
        assert!(w.len() == 8 && w[0] == "beam", "beam.list: bad line {line}");
        let int = |s: &str| s.parse::<i32>().unwrap_or_else(|e| panic!("{s}: {e}"));
        let arch: [f32; 28] = floats(w[5]);
        let shape = shape(int(w[1]), int(w[2]), int(w[3]), &arch);
        let [progress, lifetime, progress_denom] = floats(w[6]);
        let mut state = BeamState {
            flags: u8::from_str_radix(w[4], 16).unwrap_or_else(|e| panic!("{}: {e}", w[4])),
            progress,
            lifetime,
            progress_denom,
        };
        let world = Transform::from_words(floats(w[7]));
        batch.reset();
        let built = build_geometry(&mut state, &shape, &world, frame, &mut batch);
        let head = format!("{{\"case\":\"{case}\",");
        out.push(format!(
            "{head}\"r\":\"{}\",\"flags\":\"{}\",\"nv\":\"{}\",\"ni\":\"{}\"}}",
            u8::from(built),
            state.flags,
            batch.vertices.len(),
            batch.indices.len()
        ));
        for (i, v) in batch.vertices.iter().enumerate() {
            out.push(format!(
                "{head}\"vtx\":\"{i}\",\"v\":\"{},{},{},{:08x},{},{}\"}}",
                f32_hex(v.x),
                f32_hex(v.y),
                f32_hex(v.z),
                u32::from_le_bytes(v.color),
                f32_hex(v.u),
                f32_hex(v.v)
            ));
        }
        let indices: Vec<String> = batch.indices.iter().map(u16::to_string).collect();
        out.push(format!("{head}\"idx\":\"{}\"}}", indices.join(",")));
    }
    out
}
