//! Oracle vectors of work package P2.5 (the flight AI without an object
//! world): ray and box tests, steering frames, turn throttles, the state
//! graphs, the behaviour names and the personality defaults. The inputs are
//! under `tests/fixtures/oracle/`, written by `make_ai_fixtures.py`; the C++
//! side is `CppSource/Oracle/oracle_ai.cpp`, run by `Oracle.exe game-sim`
//! over Common's exported wrappers (`CppSource/include/oracle_ai.h`).
//!
//! Every suite is compared as text except `ai_turn`, whose values went
//! through sines, cosines, arc sines and arc cosines (decision T11); its
//! floats are compared within a bound.

mod support;

use std::fs;

use game_sim::ai::personality::Personality;
use game_sim::ai::raycast::{
    BoxHit, Capsule, OrientedBox, build_perpendicular_basis, clamped_dot3, point_in_capsule,
    ray_aabb_intersect, ray_oriented_box_intersect, ray_segment_capsule_first_hit,
    ray_segment_capsule_intersect, ray_sphere_intersect, ray_transformed_box_nearest_hit,
    swept_sphere_collision_time, transformed_boxes_intersect,
};
use game_sim::ai::state_graph::{StateGraphDb, behavior_id, behavior_name};
use game_sim::ai::steer::{
    build_orientation_basis, build_perpendicular_frame, build_random_spread_frame,
    get_angular_throttle, random_vector, rotate_toward_by_slerp, steering_frame_from_relative_dir,
    transform_vector_by_matrix,
};
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::rng::CrtRand;

use support::ai::{d, f, fs, state_graph_lines};
use support::data::Tolerance;
use support::render::{Line, json_text};
use support::{check_suite, f32_hex, fixtures, hex, input_lines};

fn v3(v: Vector) -> String {
    fs(&v.to_words())
}

fn vec_at(values: &[f32], at: usize) -> Vector {
    Vector::new(values[at], values[at + 1], values[at + 2])
}

fn mat_at(values: &[f32], at: usize) -> Matrix {
    Matrix::from_words(core::array::from_fn(|i| values[at + i]))
}

fn b(value: bool) -> String {
    u8::from(value).to_string()
}

fn floats(line: &str) -> Vec<f32> {
    line.split(' ').map(f32_hex).collect()
}

/// `ai_geometry.list`: offsets of the 66 values (make_ai_fixtures.py).
mod at {
    pub const ORIGIN: usize = 0;
    pub const DIR: usize = 3;
    pub const CENTER: usize = 6;
    pub const RADIUS: usize = 9;
    pub const XFORM_A: usize = 10;
    pub const BOX_A: usize = 22;
    pub const XFORM_B: usize = 28;
    pub const BOX_B: usize = 40;
    pub const CAPSULE: usize = 46;
    pub const POINT: usize = 53;
    pub const A1: usize = 56;
    pub const B0: usize = 59;
    pub const B1: usize = 62;
    pub const RADIUS_B: usize = 65;
}

fn aabb_at(values: &[f32], at: usize) -> [f32; 6] {
    core::array::from_fn(|i| values[at + i])
}

fn transform_at(values: &[f32], at: usize) -> Transform {
    Transform::from_words(core::array::from_fn(|i| values[at + i]))
}

#[test]
fn ai_raycast() {
    let mut lines = Vec::new();
    for (case, line) in input_lines("ai_geometry.list").iter().enumerate() {
        let g = floats(line);
        assert_eq!(g.len(), 66, "ai_geometry.list line {case}");
        let origin = vec_at(&g, at::ORIGIN);
        let dir = vec_at(&g, at::DIR);
        let center = vec_at(&g, at::CENTER);
        let point = vec_at(&g, at::POINT);
        let mut l = Line::new();
        l.add("in", case.to_string());

        let sphere = ray_sphere_intersect(origin, dir, center, g[at::RADIUS]);
        l.add(
            "sphere",
            format!("{},{}", b(sphere.is_some()), f(sphere.unwrap_or(-2.0))),
        );

        let mut t_min = 0.0_f32;
        let mut t_max = f32::MAX;
        let ok = ray_aabb_intersect(origin, dir, center, &mut t_min, &mut t_max);
        l.add("aabb", format!("{},{},{}", b(ok), f(t_min), f(t_max)));

        let obb = OrientedBox {
            center,
            axis_x: vec_at(&g, at::XFORM_A),
            axis_y: vec_at(&g, at::XFORM_A + 3),
            axis_z: vec_at(&g, at::XFORM_A + 6),
            half_extent: vec_at(&g, at::BOX_A),
        };
        let hit = ray_oriented_box_intersect(origin, dir, &obb);
        let (p0, p1) = match hit {
            BoxHit::Miss => (point, point),
            BoxHit::Inside(exit) => (exit, point),
            BoxHit::Through(entry, exit) => (entry, exit),
        };
        l.add(
            "obox",
            format!(
                "{},{},{},{}",
                b(hit != BoxHit::Miss),
                hit.hit_type(),
                v3(p0),
                v3(p1)
            ),
        );

        let xform_a = transform_at(&g, at::XFORM_A);
        let box_a = aabb_at(&g, at::BOX_A);
        let nearest = ray_transformed_box_nearest_hit(origin, dir, &xform_a, &box_a);
        l.add(
            "nearest",
            match nearest {
                Some(n) => format!("1,{},{}", b(n.starts_inside), v3(n.point)),
                None => format!("0,0,{}", v3(point)),
            },
        );

        let boxes = transformed_boxes_intersect(
            &xform_a,
            &box_a,
            &transform_at(&g, at::XFORM_B),
            &aabb_at(&g, at::BOX_B),
        );
        l.add("boxes", b(boxes));

        let mut d_copy = dir;
        let (a1, a2) = build_perpendicular_basis(&mut d_copy, false);
        l.add("basis", format!("{},{},{}", v3(d_copy), v3(a1), v3(a2)));
        let mut d_copy = dir;
        let (n1, n2) = build_perpendicular_basis(&mut d_copy, true);
        l.add("basis_n", format!("{},{}", v3(n1), v3(n2)));

        let capsule = Capsule {
            radius: g[at::CAPSULE],
            start: vec_at(&g, at::CAPSULE + 1),
            dir: vec_at(&g, at::CAPSULE + 4),
        };
        let ts = ray_segment_capsule_intersect(origin, dir, &capsule);
        let mut text = ts.len().to_string();
        for t in &ts {
            text.push(',');
            text.push_str(&f(*t));
        }
        l.add("capsule", text);

        let first = ray_segment_capsule_first_hit(origin, dir, &capsule);
        l.add(
            "first",
            format!(
                "{},{}",
                b(first.is_some()),
                v3(first.unwrap_or(Vector::ZERO))
            ),
        );
        l.add("in_capsule", b(point_in_capsule(point, &capsule)));
        l.add("dot", d(clamped_dot3(dir, point)));

        let swept = swept_sphere_collision_time(
            g[at::RADIUS],
            origin,
            vec_at(&g, at::A1),
            g[at::RADIUS_B],
            vec_at(&g, at::B0),
            vec_at(&g, at::B1),
        );
        l.add("swept", format!("{},{}", b(swept.hit), f(swept.t)));
        lines.push(l.done());
    }
    check_suite("ai_raycast.jsonl", &lines);
}

/// One `ai_steer.list` line: the seed and the 28 values.
struct SteerCase {
    seed: u32,
    reference: Matrix,
    forward: Vector,
    hint: Vector,
    threshold: f32,
    angle: f32,
    mat_b: Matrix,
    spread: f32,
    bank: f32,
}

fn steer_cases() -> Vec<SteerCase> {
    input_lines("ai_steer.list")
        .iter()
        .map(|line| {
            let (seed, rest) = line.split_once(' ').unwrap_or((line, ""));
            let v = floats(rest);
            assert_eq!(v.len(), 28, "ai_steer.list: {line}");
            SteerCase {
                seed: seed.parse().unwrap_or_else(|e| panic!("{line}: {e}")),
                reference: mat_at(&v, 0),
                forward: vec_at(&v, 9),
                hint: vec_at(&v, 12),
                threshold: v[15],
                angle: v[16],
                mat_b: mat_at(&v, 17),
                spread: v[26],
                bank: v[27],
            }
        })
        .collect()
}

#[test]
fn ai_steer() {
    let mut lines = Vec::new();
    for c in steer_cases() {
        let mut l = Line::new();
        l.add("seed", c.seed.to_string());
        l.add(
            "basis_hint",
            fs(&build_orientation_basis(&c.reference, c.forward, Some(c.hint)).to_words()),
        );
        l.add(
            "basis",
            fs(&build_orientation_basis(&c.reference, c.forward, None).to_words()),
        );
        for (key, reference) in [("perp", Some(c.hint)), ("perp_default", None)] {
            let frame = build_perpendicular_frame(c.forward, c.threshold, reference);
            l.add(
                key,
                format!(
                    "{},{}",
                    b(frame.is_some()),
                    fs(&frame.unwrap_or(c.reference).to_words())
                ),
            );
        }
        l.add(
            "steering",
            fs(&steering_frame_from_relative_dir(&c.reference, c.forward, c.hint).to_words()),
        );
        l.add(
            "xform",
            v3(transform_vector_by_matrix(&c.reference, c.hint)),
        );
        let mut rand = CrtRand::with_seed(c.seed);
        l.add("random", v3(random_vector(&mut rand, c.angle)));
        lines.push(l.done());
    }
    check_suite("ai_steer.jsonl", &lines);
}

/// Tier B: the slerp, the turn throttle and the spread frame within 64
/// ulps or 1e-6 (the trigonometry of decision T11).
#[test]
fn ai_turn() {
    let mut lines = Vec::new();
    for c in steer_cases() {
        let mut l = Line::new();
        l.add("seed", c.seed.to_string());
        l.add(
            "slerp",
            v3(rotate_toward_by_slerp(c.forward, c.hint, c.angle)),
        );
        let throttle = get_angular_throttle(&c.reference, &c.mat_b);
        l.add(
            "throttle",
            format!("{},{}", v3(throttle.throttle), f(throttle.angle)),
        );
        let mut rand = CrtRand::with_seed(c.seed);
        l.add(
            "spread",
            fs(&build_random_spread_frame(&mut rand, &c.reference, c.spread, c.bank).to_words()),
        );
        lines.push(l.done());
    }
    let name = "ai_turn.jsonl";
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, &lines);
        return;
    }
    let tolerance = Tolerance {
        keys: &["slerp", "throttle", "spread"],
        ulps: 64,
        absolute: 1.0e-6,
    };
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  oracle: {want}\n  rust:   {got}",
                index + 1
            );
        }
    }
}

#[test]
fn ai_state_graph() {
    let mut lines = Vec::new();
    for file in input_lines("ai_state_graph.list") {
        let path = fixtures().join("ai_state_graph").join(&file);
        let bytes = fs::read(&path).ok();
        let db = StateGraphDb::load(bytes.as_deref()).unwrap_or_default();
        lines.push(
            Line::new()
                .add("file", file.as_str())
                .add("graphs", db.graphs.len().to_string())
                .done(),
        );
        lines.extend(state_graph_lines(&db));
    }
    check_suite("ai_state_graph.jsonl", &lines);
}

#[test]
fn ai_behavior_names() {
    let mut lines = Vec::new();
    for id in -1..=20 {
        let name = behavior_name(id).unwrap_or("");
        lines.push(
            Line::new()
                .add("id", id.to_string())
                .add("name", json_text(name.as_bytes()))
                .done(),
        );
    }
    for name in input_lines("ai_behavior_names.list") {
        lines.push(
            Line::new()
                .add("name", json_text(name.as_bytes()))
                .add("id", behavior_id(name.as_bytes()).to_string())
                .done(),
        );
    }
    check_suite("ai_behavior_names.jsonl", &lines);
}

#[test]
fn ai_personality() {
    let mut lines = vec![
        Line::new()
            .add("default", hex(&Personality::default().to_bytes()))
            .done(),
    ];
    for kind in -1..=6 {
        lines.push(
            Line::new()
                .add("toughness", kind.to_string())
                .add("ratio", f(Personality::toughness_ratio(kind)))
                .done(),
        );
    }
    for job in -1..=3 {
        let mut p = Personality::default();
        p.job.unknown_00 = job;
        lines.push(
            Line::new()
                .add("job", job.to_string())
                .add("range", d(p.range_threshold_value()))
                .done(),
        );
    }
    check_suite("ai_personality.jsonl", &lines);
}
