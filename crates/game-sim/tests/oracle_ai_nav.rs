//! Oracle vectors of work package P2.5's navigation layer: the intercept
//! and clearance queries (`game_sim::ai::intercept`) and the
//! obstacle-avoidance helpers (`game_sim::ai::avoidance`). The inputs are
//! `tests/fixtures/oracle/ai_nav.list`, written by `make_ai_nav_fixtures.py`;
//! the C++ side is `CppSource/Oracle/oracle_ai_nav.cpp`, run by
//! `Oracle.exe game-sim` over Common's exported wrappers
//! (`CppSource/include/oracle_ai.h`), which answer each object through an
//! `IObjInspect` built from the same record.
//!
//! `ai_nav` is compared as text. `ai_nav_turn` holds
//! `NudgeObstaclePathPoint`, whose last branch turns through a sine and a
//! cosine (decision T11), and `ai_idea` the movement ideas' helpers
//! (`MoveIdea::SetDirection`, `LimitAxisThrottleForTurn`,
//! `ComputeBankedSteeringFrame`), which go through arc cosines; their
//! floats are compared within a bound.

mod support;

use std::fs;

use game_sim::ai::avoidance::{
    AvoidSlot, SLOT_COUNT, average_active_direction, nudge_obstacle_path_point,
    resolve_obstacle_candidate_slot,
};
use game_sim::ai::bank::{compute_banked_steering_frame, limit_axis_throttle_for_turn};
use game_sim::ai::intercept::{
    estimate_lead_intercept_distance, estimate_turn_circle_approach_cost, get_intercept_clearance,
    get_intercept_clearance_of_iface, iface_vector_magnitude, is_aligned_and_outside_heading_cone,
    is_point_outside_heading_cone, is_target_co_aligned_and_ahead, is_within_intercept_range,
    is_within_intercept_range_of_iface, shape_biased_avoid_direction,
    signed_radial_clearance_to_point, transform_bounding_box_corners,
};
use game_sim::ai::move_idea::MoveIdea;
use game_sim::ai::world::{MotionModel, ObjectView, ThrustEquipType};
use neuron_core::math::{Matrix, Transform, Vector};

use support::ai::{d, f, fs};
use support::check_suite;
use support::data::Tolerance;
use support::input_lines;
use support::render::Line;

/// One object record (34 words) of `ai_nav.list`, as the C++ fake answers
/// it.
struct Object {
    view: ObjectView,
    angular_speed_xy: f32,
    speed: f32,
    /// The record's extents, which the C++ suite hands to
    /// `TransformBoundingBoxCorners` even when the fake reports a failure.
    raw_extents: (Vector, Vector),
}

impl MotionModel for Object {
    fn time_to_accelerate(&self, _: f32, _: f32, _: f32, _: ThrustEquipType) -> f32 {
        0.0
    }
    fn distance_travelled(&self, _: f32, _: f32, _: f32, _: ThrustEquipType) -> f32 {
        0.0
    }
    fn projected_throttle(&self, _: f32, _: ThrustEquipType) -> f32 {
        0.0
    }
    fn speed(&self, _: f32, _: ThrustEquipType) -> f32 {
        self.speed
    }
    fn initial_speed_to_coast_distance(&self, _: f32, _: bool) -> f32 {
        0.0
    }
    fn time_to_accelerate_angularly(&self, _: f32, _: f32, _: f32) -> f32 {
        0.0
    }
    fn time_to_accelerate_angularly_xy(&self, _: f32, _: f32, dt: f32, max: f32) -> f32 {
        dt + max
    }
    fn angular_distance_travelled_xy(&self, _: f32, _: f32, dt: f32, max: f32) -> f32 {
        dt * max
    }
    fn angular_distance_travelled(&self, _: f32, to: f32, dt: f32) -> f32 {
        to * dt
    }
    fn angular_speed_xy(&self, _: f32, _: f32) -> f32 {
        self.angular_speed_xy
    }
    fn angular_speed_z(&self, speed: f32) -> f32 {
        speed * self.angular_speed_xy
    }
    fn projected_axis_throttle_xy(&self, speed: f32) -> f32 {
        speed * 0.5
    }
    fn projected_axis_throttle_z(&self, _: f32) -> f32 {
        0.0
    }
}

fn fl(words: &[u32], at: usize) -> f32 {
    f32::from_bits(words[at])
}

fn vec_at(words: &[u32], at: usize) -> Vector {
    Vector::new(fl(words, at), fl(words, at + 1), fl(words, at + 2))
}

fn object(words: &[u32]) -> Object {
    let position = vec_at(words, 0);
    let orientation = Matrix::from_words(core::array::from_fn(|i| fl(words, 3 + i)));
    let extents_fail = words[29] != 0;
    let view = ObjectView {
        id: 1,
        type_flags: words[28],
        position,
        orientation,
        velocity: vec_at(words, 12),
        angular_velocity: vec_at(words, 15),
        radius: fl(words, 18),
        center: vec_at(words, 19),
        extents: (!extents_fail).then(|| (vec_at(words, 22), vec_at(words, 25))),
        throttle: Some(fl(words, 30)),
        atmosphere_range: fl(words, 33),
        axis_throttle: Some(vec_at(words, 15)),
        max_bank_angle: 0.6,
        ..ObjectView::default()
    };
    Object {
        view,
        angular_speed_xy: fl(words, 31),
        speed: fl(words, 32),
        raw_extents: (vec_at(words, 22), vec_at(words, 25)),
    }
}

/// One `ai_nav.list` line.
struct Case {
    a: Object,
    b: Object,
    point: Vector,
    threshold: f32,
    cos: f32,
    angular: f32,
    linear: f32,
    slots: [AvoidSlot; 3],
    extra_check: bool,
}

fn cases() -> Vec<Case> {
    input_lines("ai_nav.list")
        .iter()
        .map(|line| {
            let w: Vec<u32> = line
                .split(' ')
                .map(|t| u32::from_str_radix(t, 16).unwrap_or_else(|e| panic!("{t}: {e}")))
                .collect();
            assert_eq!(w.len(), 97, "ai_nav.list: {line}");
            let slot = |at: usize| AvoidSlot {
                timer: fl(&w, at),
                point: vec_at(&w, at + 1),
                direction: vec_at(&w, at + 4),
            };
            Case {
                a: object(&w[0..34]),
                b: object(&w[34..68]),
                point: vec_at(&w, 68),
                threshold: fl(&w, 71),
                cos: fl(&w, 72),
                angular: fl(&w, 73),
                linear: fl(&w, 74),
                slots: [slot(75), slot(82), slot(89)],
                extra_check: w[96] != 0,
            }
        })
        .collect()
}

fn b(value: bool) -> String {
    u8::from(value).to_string()
}

fn v3(v: Vector) -> String {
    fs(&v.to_words())
}

#[test]
fn ai_nav() {
    let mut lines = Vec::new();
    for (index, c) in cases().iter().enumerate() {
        let (a, bv) = (&c.a.view, &c.b.view);
        let mut l = Line::new();
        l.add("in", index.to_string());
        l.add("lead", d(estimate_lead_intercept_distance(a, bv)));
        l.add("clear", d(get_intercept_clearance(a, bv)));
        l.add("range", b(is_within_intercept_range(a, bv, c.threshold)));
        l.add("clear_if", d(get_intercept_clearance_of_iface(a, bv)));
        l.add(
            "range_if",
            b(is_within_intercept_range_of_iface(a, bv, c.threshold)),
        );
        l.add(
            "cone",
            format!(
                "{},{}",
                b(is_point_outside_heading_cone(c.point, Some(bv), c.cos)),
                b(is_point_outside_heading_cone(c.point, None, c.cos))
            ),
        );
        l.add(
            "aligned",
            format!(
                "{},{}",
                b(is_aligned_and_outside_heading_cone(a, Some(bv), c.cos)),
                b(is_aligned_and_outside_heading_cone(a, None, c.cos))
            ),
        );
        l.add("radial", d(signed_radial_clearance_to_point(a, c.point)));
        l.add("ahead", b(is_target_co_aligned_and_ahead(bv, a, c.cos)));
        l.add(
            "speed",
            format!(
                "{},{}",
                d(iface_vector_magnitude(Some(bv))),
                d(iface_vector_magnitude(None))
            ),
        );
        let (min, max) = c.b.raw_extents;
        let corners = transform_bounding_box_corners(&bv.transform(), min, max);
        let words: Vec<f32> = corners.iter().flat_map(|v| v.to_words()).collect();
        l.add("corners", fs(&words));
        l.add(
            "avoid",
            format!(
                "{},{}",
                v3(shape_biased_avoid_direction(a, Some(bv))),
                v3(shape_biased_avoid_direction(a, None))
            ),
        );
        let motion: &dyn MotionModel = &c.a;
        l.add(
            "turn",
            format!(
                "{},{}",
                d(estimate_turn_circle_approach_cost(
                    a,
                    Some(motion),
                    c.point,
                    c.angular,
                    c.linear
                )),
                d(estimate_turn_circle_approach_cost(
                    a,
                    Some(motion),
                    c.point,
                    0.0,
                    0.0
                ))
            ),
        );
        let mut slots = [AvoidSlot::default(); SLOT_COUNT];
        slots[..3].copy_from_slice(&c.slots);
        let average = average_active_direction(&slots, &a.transform());
        l.add(
            "average",
            format!(
                "{},{}",
                b(average.is_some()),
                v3(average.unwrap_or(Vector::ZERO))
            ),
        );
        let slot = resolve_obstacle_candidate_slot(bv);
        l.add(
            "slot",
            format!(
                "1,{},{},{}",
                f(slot.timer),
                v3(slot.point),
                v3(slot.direction)
            ),
        );
        lines.push(l.done());
    }
    check_suite("ai_nav.jsonl", &lines);
}

/// Tier B: the nudge within 64 ulps or 1e-5 (the slerp's trigonometry).
#[test]
fn ai_nav_turn() {
    let mut lines = Vec::new();
    for (index, c) in cases().iter().enumerate() {
        let mut l = Line::new();
        l.add("in", index.to_string());
        let obstacle: Transform = c.b.view.transform();
        for (slot, key) in [(0, "nudge0"), (1, "nudge1")] {
            let extra = if slot == 0 {
                c.extra_check
            } else {
                !c.extra_check
            };
            let nudge = nudge_obstacle_path_point(&c.slots[slot], c.point, &obstacle, extra);
            let point = match nudge {
                game_sim::ai::avoidance::Nudge::Point(p) => p,
                _ => Vector::ZERO,
            };
            l.add(key, format!("{},{}", nudge.code(), v3(point)));
        }
        lines.push(l.done());
    }
    check_within("ai_nav_turn.jsonl", &lines, &["nudge0", "nudge1"]);
}

/// Compares `lines` with the committed suite, the floats of `keys` within
/// 64 ulps or 1e-5 (or writes it when blessing).
fn check_within(name: &str, lines: &[String], keys: &'static [&'static str]) {
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, lines);
        return;
    }
    let tolerance = Tolerance {
        keys,
        ulps: 64,
        absolute: 1.0e-5,
    };
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  oracle: {want}\n  rust:   {got}",
                index + 1
            );
        }
    }
}

/// Tier B in `limit` and `bank`: the movement ideas' helpers.
#[test]
fn ai_idea() {
    let mut lines = Vec::new();
    for (index, c) in cases().iter().enumerate() {
        let mut l = Line::new();
        l.add("in", index.to_string());
        let mut idea = MoveIdea::<u32>::new();
        idea.set_direction(c.point);
        l.add("dir", v3(idea.direction));
        idea.set_direction_from(&c.a.view.orientation);
        l.add("dir_from", v3(idea.direction));

        let a = &c.a.view;
        let motion: &dyn MotionModel = &c.a;
        let ref_angle = f32_mul(c.threshold, 0.01);
        let target = c.b.view.orientation;
        let limit: Vec<String> = [false, true]
            .iter()
            .map(|&bank| {
                v3(limit_axis_throttle_for_turn(
                    Some(a),
                    Some(motion),
                    c.b.view.velocity,
                    &target,
                    c.angular,
                    ref_angle,
                    bank,
                ))
            })
            .collect();
        l.add("limit", limit.join(","));

        let m = target.to_words();
        let target_dir = Vector::new(m[1], m[4], m[7]);
        let banked: Vec<String> = [0_u8, 1]
            .iter()
            .map(|&mode| {
                let mut frame = target;
                let mut bank_vector = c.b.view.angular_velocity;
                let steered = compute_banked_steering_frame(
                    a,
                    Some(motion),
                    target_dir,
                    &mut frame,
                    &mut bank_vector,
                    mode,
                );
                format!(
                    "{},{},{}",
                    b(steered),
                    fs(&frame.to_words()),
                    v3(bank_vector)
                )
            })
            .collect();
        l.add("bank", banked.join(","));
        lines.push(l.done());
    }
    check_within("ai_idea.jsonl", &lines, &["limit", "bank"]);
}

/// A `float` product.
fn f32_mul(a: f32, b: f32) -> f32 {
    (f64::from(a) * f64::from(b)) as f32
}
