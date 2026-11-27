//! The lines of `CppSource/Oracle/oracle_beam.cpp`: each `beam` case of
//! `beam.list` built by `render::effects::beam::build_geometry` into a reset
//! batch, the beam watching nothing, the `particles` option on and the
//! cockpit camera not current (the process's stand-ins).

use neuron_client::render::effects::batch::EffectVertexBatch;
use neuron_client::render::effects::beam::{
    BEAM_EFFECT_TYPE, BeamEffect, BeamFrame, BeamShape, BeamState, build_geometry,
};
use neuron_client::render::effects::set::{
    EFFECT_TYPE_BUCKETS, EffectCamera, EffectHost, EffectId, EffectSet, EffectTypeParams, WalkFrame,
};
use neuron_core::math::Transform;
use neuron_core::math::Vector;

pub fn f32_hex(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

pub fn floats<const N: usize>(text: &str) -> [f32; N] {
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
pub fn shape(mode: i32, head: i32, trail: i32, a: &[f32; 28]) -> BeamShape {
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

/// No engine instance and no watched object: the walk's beams are free.
struct NoEngine;

impl EffectHost for NoEngine {
    fn instance_transform(&self, _instance: i32) -> Transform {
        Transform::IDENTITY
    }
    fn instance_velocity(&self, _instance: i32) -> Option<Vector> {
        None
    }
    fn watched(&self, _watch: u32) -> bool {
        false
    }
}

const DEFAULT_TYPE: EffectTypeParams = EffectTypeParams {
    priority: 0.0,
    visibility_offscreen: false,
    pbubble_near: 0.0,
    pbubble_far: 0.0,
};

/// The lines of `RunBeamWalkSuite` (`beam_walk.list`).
pub fn walk_lines(input: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input.iter().enumerate() {
        let mut set: EffectSet<BeamEffect> = EffectSet::default();
        let mut beams: Vec<(usize, EffectId)> = Vec::new();
        let mut types = [DEFAULT_TYPE; EFFECT_TYPE_BUCKETS];
        let mut cam = EffectCamera {
            view: Transform::IDENTITY,
            z_near: 0.0,
            plane_x: [0.0; 3],
            plane_y: [0.0; 3],
        };
        let mut batch = EffectVertexBatch::default();
        let mut walk = 0;
        let id_of = |beams: &[(usize, EffectId)], i: usize| {
            beams
                .iter()
                .find(|(k, _)| *k == i)
                .map(|(_, id)| *id)
                .unwrap_or_else(|| panic!("beam {i} not made"))
        };
        for op in line.split(';') {
            let w: Vec<&str> = op.split_whitespace().collect();
            let Some(&name) = w.first() else { continue };
            let frame = WalkFrame {
                host: &NoEngine,
                types: &types,
                generic_effect_perf: 1.0,
                particles: true,
                first_person: false,
                server_paused: false,
                beam_texture: true,
            };
            match (name, w.len()) {
                ("cam", 5) => {
                    cam.view = Transform::from_words(floats(w[1]));
                    [cam.z_near] = floats(w[2]);
                    cam.plane_x = floats(w[3]);
                    cam.plane_y = floats(w[4]);
                }
                ("type", 5) => {
                    let [near] = floats(w[2]);
                    let [far] = floats(w[3]);
                    let [priority] = floats(w[4]);
                    types[BEAM_EFFECT_TYPE] = EffectTypeParams {
                        priority,
                        visibility_offscreen: w[1] != "0",
                        pbubble_near: near,
                        pbubble_far: far,
                    };
                }
                ("beam", 9) => {
                    let i: usize = w[1].parse().expect("the beam's number");
                    let mode: i32 = w[2].parse().expect("the mode");
                    let mut beam = BeamEffect::new(BEAM_EFFECT_TYPE);
                    beam.shape = Some(shape(mode, 0, 0, &floats(w[4])));
                    let [progress, lifetime, progress_denom] = floats(w[5]);
                    beam.state = BeamState {
                        flags: u8::from_str_radix(w[3], 16).expect("the flags"),
                        progress,
                        lifetime,
                        progress_denom,
                    };
                    beam.base.local = Transform::from_words(floats(w[6]));
                    let [x, y, z] = floats(w[7]);
                    beam.base.velocity = Vector::new(x, y, z);
                    [beam.base.priority] = floats(w[8]);
                    let id = set.insert(beam);
                    // A slot used again names the new beam only.
                    beams.retain(|(_, e)| *e != id);
                    beams.push((i, id));
                }
                ("link", 2) | ("release", 2) | ("kill", 2) => {
                    let id = id_of(&beams, w[1].parse().expect("the beam's number"));
                    match name {
                        "link" => set.link(id),
                        "release" => {
                            set.release(id);
                        }
                        _ => set.kill(id, &frame),
                    }
                }
                ("walk", 2) => {
                    let [dt] = floats(w[1]);
                    batch.reset();
                    set.walk(BEAM_EFFECT_TYPE, dt, &cam, &frame, &mut batch);
                    let head = format!("{{\"case\":\"{case}\",\"walk\":\"{walk}\",");
                    let order: Vec<usize> = set
                        .bucket(BEAM_EFFECT_TYPE)
                        .iter()
                        .filter_map(|id| beams.iter().find(|(_, e)| e == id).map(|(k, _)| *k))
                        .collect();
                    let list: Vec<String> = order.iter().map(usize::to_string).collect();
                    out.push(format!("{head}\"bucket\":\"{}\"}}", list.join(",")));
                    for i in order {
                        let b = set.get(id_of(&beams, i)).expect("a linked beam lives");
                        let t = b.base.local.translation;
                        out.push(format!(
                            "{head}\"beam\":\"{i}\",\"v\":\"{},{},{},{},{},{},{},{}\"}}",
                            b.base.flags,
                            b.state.flags,
                            f32_hex(b.state.progress),
                            f32_hex(b.state.lifetime),
                            f32_hex(t.x),
                            f32_hex(t.y),
                            f32_hex(t.z),
                            b.base.ref_count
                        ));
                    }
                    out.push(format!(
                        "{head}\"nv\":\"{}\",\"ni\":\"{}\"}}",
                        batch.vertices.len(),
                        batch.indices.len()
                    ));
                    walk += 1;
                }
                _ => panic!("beam_walk.list: bad operation '{op}'"),
            }
        }
        let frame = WalkFrame {
            host: &NoEngine,
            types: &types,
            generic_effect_perf: 1.0,
            particles: true,
            first_person: false,
            server_paused: false,
            beam_texture: true,
        };
        set.kill_all(&frame);
    }
    out
}
