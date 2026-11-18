//! `SplineMoveIdea` (`Common/common_splinemoveidea.cpp`, class in
//! `include/common_idea.h`): flies a ship along one Catmull-Rom segment
//! with an embedded [`MoveIdea`] toward the segment's end and an embedded
//! [`FaceIdea`] toward a point ahead on the curve, done once past the end.
//!
//! The segment is [`neuron_core::util::spline::CrSplineSegment`]. The
//! system manager's `ComputeFormationFollowerEnvelope` result is an
//! argument of [`SplineMoveIdea::update`] ([`FollowerEnvelope`]); the
//! [`Idea`] implementation computes it with
//! [`super::goto::formation_follower_envelope`].

use neuron_core::math::Vector;
use neuron_core::util::spline::{CrSplineSegment, Vec3};

use super::behavior::{AiEnv, Cx};
use super::face_idea::FaceIdea;
use super::fp::{add, column2, e, neg, normalize3, r, scale, sub};
use super::idea::{Idea, IdeaBase};
use super::intercept::is_point_outside_heading_cone;
use super::move_idea::{MoveIdea, status as move_status};
use super::nav::{NavCx, projected_throttle, query_fixed_falloff_effect, speed, with_own_motion};
use super::raycast::clamped_dot3;
use super::world::{AiWorld, MotionModel, ThrustEquipType};

/// `SplineMoveIdea::GetKind`.
pub const SPLINE_MOVE_IDEA_KIND: i16 = 0x16;

/// The value `Update` stores in the pathfinder's `+0x248`.
const FIELD_248_SPLINE: i32 = 0x3003_8010;

/// `FormationFollowerEnvelope` as `SplineMoveIdea::Update` reads it from
/// `GunConvergenceStateBytes.ComputeFormationFollowerEnvelope`: whether it
/// was computed, and its throttle (0 when the call leaves it as seeded).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FollowerEnvelope {
    /// The call's result.
    pub valid: bool,
    /// `Envelope.Throttle`.
    pub throttle: f32,
}

/// `SplineMoveIdea` (`common_idea.h`), its fields from `+0x10`.
#[derive(Clone, Debug, PartialEq)]
pub struct SplineMoveIdea {
    /// `BaseIdea`'s fields (+0x10 `Attached`).
    pub base: IdeaBase,
    /// +0x14 the embedded move idea.
    pub move_idea: MoveIdea,
    /// +0x8c the embedded face idea.
    pub face: FaceIdea,
    /// +0x14c the segment.
    pub spline: Option<CrSplineSegment>,
    /// +0x150 cruise along the segment.
    pub wide_mode: u8,
    /// +0x154 the throttle cap (1).
    pub bank_blend_cap: f32,
    /// +0x158 the move idea's stop style once done.
    pub field_158: i32,
    /// +0x15c `get_speed(1, engine)` at `SetSpline`.
    pub max_speed_15c: f32,
    /// +0x160 how far ahead the curve is sampled.
    pub lookahead_scale: f32,
    /// +0x164 the segment length.
    pub speed_norm: f32,
    /// +0x168 (10).
    pub step_numerator: f32,
    /// +0x16c (50).
    pub extrapolate_base: f32,
    /// +0x170 the parameter of the ship's place on the curve.
    pub persisted_t: f32,
}

impl Default for SplineMoveIdea {
    fn default() -> Self {
        Self::new()
    }
}

fn v(p: Vec3) -> Vector {
    Vector::new(p[0], p[1], p[2])
}

/// `slot DC` (`Behavior_InvokeManagerSlotDC`, `common_behavior.cpp:195`):
/// `get_initial_speed_to_coast_distance(value, true)`, 0 for 0.
fn coast_speed(motion: Option<&dyn MotionModel>, value: f32) -> f32 {
    match motion {
        Some(m) if value != 0.0 => m.initial_speed_to_coast_distance(value, true),
        _ => 0.0,
    }
}

impl SplineMoveIdea {
    /// `SplineMoveIdea::SplineMoveIdea` (`common_splinemoveidea.cpp:34`).
    #[must_use]
    pub fn new() -> Self {
        Self {
            base: IdeaBase::default(),
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            spline: None,
            wide_mode: 0,
            bank_blend_cap: 1.0,
            field_158: 0,
            max_speed_15c: 0.0,
            lookahead_scale: 0.0,
            speed_norm: 0.0,
            step_numerator: 10.0,
            extrapolate_base: 50.0,
            persisted_t: 0.0,
        }
    }

    /// `SplineMoveIdea::SetContext` (`common_splinemoveidea.cpp:58`).
    pub fn set_context<C: NavCx + ?Sized>(&mut self, cx: &mut C) -> bool {
        let Some(view) = cx.world().inspect(cx.ship()) else {
            return false;
        };
        self.base.owner = Some(view.id);
        self.base.attached = true;
        self.move_idea.set_context(&*cx);
        self.face.set_context(&*cx);
        self.spline = None;
        cx.pathfind_mut().target_id = 0;
        self.persisted_t = 0.0;
        true
    }

    /// `SplineMoveIdea::Commit` (`common_splinemoveidea.cpp:92`).
    pub fn commit<C: NavCx + ?Sized>(&mut self, cx: &mut C) {
        cx.pathfind_mut().target_id = 0;
        self.move_idea.commit(cx);
        self.face.commit(cx);
        cx.set_nudge_direction(Vector::ZERO);
        self.persisted_t = 0.0;
    }

    /// `SplineMoveIdea::SetBankBlendCap`.
    pub fn set_bank_blend_cap(&mut self, value: f32) {
        self.bank_blend_cap = value;
    }

    /// `SplineMoveIdea::SetThrottleScale`: the engine throttle for `value`.
    pub fn set_throttle_scale<C: NavCx + ?Sized>(&mut self, cx: &C, value: f32) {
        self.bank_blend_cap = with_own_motion(cx, |m| {
            projected_throttle(m, value, ThrustEquipType::Engine)
        });
    }

    /// `SplineMoveIdea::SetField158`.
    pub fn set_field_158(&mut self, value: i32) {
        self.field_158 = value;
    }

    /// `SplineMoveIdea::SetWideMode`: cruise (2) or engine (0) for the
    /// move idea.
    pub fn set_wide_mode<C: NavCx + ?Sized>(&mut self, cx: &mut C, value: u8) {
        self.wide_mode = value;
        self.move_idea.set_mode(cx, if value != 0 { 2 } else { 0 });
    }

    /// `SplineMoveIdea::SetSpline` (`common_splinemoveidea.cpp:182`).
    pub fn set_spline<C: NavCx + ?Sized>(&mut self, cx: &C, spline: CrSplineSegment) {
        self.move_idea.arrival_range = self.extrapolate_base;
        self.move_idea.stop_style = 2;
        self.move_idea.kill_engine_off_bearing = 0;
        let (max_speed, lookahead) = with_own_motion(cx, |motion| {
            let max_speed = speed(motion, 1.0, ThrustEquipType::Engine);
            (max_speed, query_fixed_falloff_effect(motion, max_speed))
        });
        self.max_speed_15c = max_speed;
        self.lookahead_scale = lookahead;
        self.speed_norm = spline.spline_length();
        self.spline = Some(spline);
    }

    /// `SplineMoveIdea::Update` (`common_splinemoveidea.cpp:227`): 3
    /// without a context, segment or ship; 1 once the move idea arrived or
    /// the ship passed the end and the point beyond it is behind; else 0.
    ///
    /// A parameter search that cannot finish (the reference loops forever)
    /// keeps the start parameter (D163).
    pub fn update<C: NavCx + ?Sized>(
        &mut self,
        cx: &mut C,
        envelope: FollowerEnvelope,
        dt: f32,
    ) -> i32 {
        if !self.base.attached {
            return 3;
        }
        let Some(spline) = self.spline else {
            return 3;
        };
        let Some(view) = cx.world().inspect(cx.ship()) else {
            return 3;
        };
        if envelope.valid {
            self.move_idea.throttle = envelope.throttle;
        }

        let elem_pos = view.center;
        let step = r(e(self.step_numerator) / e(self.speed_norm));
        let t1 = spline
            .t_from_point(elem_pos.to_words(), self.persisted_t, step)
            .unwrap_or(self.persisted_t);
        self.persisted_t = t1;

        let raw = view.velocity;
        let raw_len = ((e(raw.y) * e(raw.y) + e(raw.z) * e(raw.z)) + e(raw.x) * e(raw.x)).sqrt();
        let dir_a = if !(raw_len < 1.0) {
            normalize3(raw)
        } else {
            neg(column2(&view.orientation))
        };
        let lookahead_point = add(elem_pos, scale(dir_a, self.lookahead_scale));
        let t2 = spline
            .t_from_point(lookahead_point.to_words(), t1, step)
            .unwrap_or(t1);

        let wide = self.wide_mode != 0;
        let mut bank_blend = 1.0_f64;
        let bonus: f32 = if wide { 200.0 } else { 50.0 };
        let t3 = r((e(view.radius) + e(bonus)) / e(self.speed_norm) + e(t2));
        let far_point = v(spline.point(t3));
        if wide {
            if self.move_idea.mode == 1 {
                cx.set_nudge_direction(Vector::ZERO);
            }
            self.move_idea.mode = 2;
        } else {
            let delta = sub(far_point, lookahead_point);
            let delta_len = ((e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z))
                + e(delta.x) * e(delta.x))
            .sqrt();
            let mut bank_factor = 1.0_f64;
            if !(delta_len < e(0.999_9)) {
                let dot = clamped_dot3(normalize3(delta), dir_a);
                bank_factor = if dot < e(0.1) { e(0.1) } else { dot };
            }
            let lookahead = self.lookahead_scale;
            bank_blend = with_own_motion(&*cx, |motion| {
                let coast = coast_speed(motion, r(bank_factor * e(lookahead)));
                e(projected_throttle(motion, coast, ThrustEquipType::Engine))
            });
        }
        if !(bank_blend < e(envelope.throttle)) {
            bank_blend = e(envelope.throttle);
        }
        if bank_blend > e(self.bank_blend_cap) {
            bank_blend = e(self.bank_blend_cap);
        }
        self.move_idea.throttle = r(bank_blend);
        self.move_idea.max_thrust_bearing = f32::from_bits(0x4049_0fdb);
        self.move_idea.target_kind = 2;
        self.move_idea.position = v(spline.p2);
        let pathfind = cx.pathfind_mut();
        pathfind.cached_target_position = self.move_idea.position;
        pathfind.field_248 = FIELD_248_SPLINE;
        let move_result = self.move_idea.update(cx, dt);

        let p2 = v(spline.p2);
        let chord = sub(v(spline.p3), p2);
        if !(e(t2) < 1.0) {
            self.face.direction_28 = chord;
            self.face.target_kind = 3;
        } else {
            self.face.position_1c = far_point;
            self.face.target_kind = 2;
        }
        let _ = self.face.update(cx);

        if move_result != move_status::ARRIVED && !(e(t1) > 1.0) {
            return 0;
        }
        let chord_dir = normalize3(chord);
        let factor = -e(self.extrapolate_base) - e(1.0);
        let sx = r(e(chord_dir.x) * factor);
        let sy = r(e(chord_dir.y) * factor);
        let probe = Vector::new(
            r(e(sx) + e(p2.x)),
            r(e(sy) + e(p2.y)),
            r(factor * e(chord_dir.z) + e(p2.z)),
        );
        if !is_point_outside_heading_cone(probe, Some(&view), 0.0) {
            return 0;
        }
        self.move_idea.stop_style = self.field_158;
        SplineMoveIdea::commit(self, cx);
        1
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for SplineMoveIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        let (valid, envelope) = super::goto::formation_follower_envelope(&*cx);
        let envelope = FollowerEnvelope {
            valid,
            throttle: envelope.throttle,
        };
        SplineMoveIdea::update(self, cx, envelope, dt)
    }

    fn kind(&self) -> i16 {
        SPLINE_MOVE_IDEA_KIND
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        SplineMoveIdea::commit(self, cx);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::move_idea::tests::{TestCx, Two};

    #[test]
    fn follows_and_finishes_a_segment() {
        let mut cx = TestCx::new(Two::new(Vector::ZERO, Vector::ZERO));
        cx.thrust.accept = true;
        let mut idea = SplineMoveIdea::new();
        let env = FollowerEnvelope {
            valid: false,
            throttle: 1.0,
        };
        assert_eq!(idea.update(&mut cx, env, 0.1), 3);
        assert!(idea.set_context(&mut cx));
        // A straight segment ahead along -Z, 0 to -1000.
        let spline = CrSplineSegment::new(
            [0.0, 0.0, 100.0],
            [0.0, 0.0, 0.0],
            [0.0, 0.0, -1000.0],
            [0.0, 0.0, -1100.0],
        );
        idea.set_spline(&cx, spline);
        assert_eq!(idea.speed_norm, 1000.0);
        assert_eq!(idea.move_idea.arrival_range, 50.0);
        assert_eq!(idea.update(&mut cx, env, 0.1), 0);
        assert_eq!(
            cx.pathfind.cached_target_position,
            Vector::new(0.0, 0.0, -1000.0)
        );
        assert_eq!(cx.pathfind.field_248, FIELD_248_SPLINE);
        assert_eq!(idea.face.target_kind, 2);
        // Past the end of the segment, with the probe behind: done.
        cx.world.ship.center = Vector::new(0.0, 0.0, -1040.0);
        cx.world.ship.position = cx.world.ship.center;
        assert_eq!(idea.update(&mut cx, env, 0.1), 1);
        assert_eq!(cx.pathfind.cached_target_position, Vector::ZERO);
    }
}
