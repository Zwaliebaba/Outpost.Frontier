//! `TrailBehavior` (behaviour 3, `Common/common_trailbehavior.cpp`, class
//! in `include/common_trailbehavior.h`): sit on a ship's tail. The trail
//! point lies `TrailDistance` behind the leader along its own axis; the
//! match-throttle idea drives while the ship is behind the leader heading
//! its way, the move idea otherwise, and the face idea keeps the leader in
//! the sights. An AI ship that has been facing its target turns away to
//! its axis for a while (the "lag" and "axis" pauses).

use neuron_core::math::{Vector, transform_matrix};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, EvaluateResult};
use super::behaviors::op_dword;
use super::face_idea::FaceIdea;
use super::fp::{e, normalize3, r, sub};
use super::idea::{AfterburnerIdea, Idea, publish_target_id};
use super::intercept::{
    get_intercept_clearance_of_iface, iface_vector_magnitude, is_aligned_and_outside_heading_cone,
};
use super::match_throttle::MatchThrottle2Idea;
use super::move_idea::MoveIdea;
use super::nav::max_scalar_attribute;
use super::raycast::{bearing_angle_to_object, clamped_dot3};
use super::target_manager::CombatEvent;
use super::timer_table::slot;
use super::world::ObjectView;

/// π/2 (`0x3fc90fdb`).
const HALF_PI: f32 = core::f32::consts::FRAC_PI_2;
/// π (`0x40490fdb`).
const PI: f32 = core::f32::consts::PI;

/// `TrailBehavior` (0x248 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct TrailBehavior {
    base: BehaviorBase,
    /// +0x2c the match-throttle idea.
    pub match_throttle: MatchThrottle2Idea,
    /// +0x88 the face idea.
    pub face: FaceIdea,
    /// +0x148 the move idea.
    pub move_idea: MoveIdea,
    /// +0x1c0 the afterburner idea.
    pub afterburner: AfterburnerIdea,
    /// +0x1d4 the directive's target.
    pub directive_target: Option<u32>,
    /// +0x1dc the ship last trailed.
    pub leader_1dc: u32,
    /// +0x1e0 a directive target is set.
    pub has_directive_target: u8,
    /// +0x1e4 the trail offset behind the leader.
    pub trail_offset: f32,
    /// +0x1e8 the trail distance (50).
    pub trail_distance: f32,
    /// +0x1ec facing the leader (the lag pause armed).
    pub lagging: u8,
    /// +0x1f0 the facing cosine (`cos(π/6)`).
    pub facing_cos: f32,
    /// +0x1f4 the lag clock.
    pub lag_timer: f32,
    /// +0x1f8 the axis clock.
    pub axis_timer: f32,
    /// +0x1fc the lag time (2.5).
    pub lag_time: f32,
    /// +0x200 the axis pause (5).
    pub axis_pause: f32,
    /// +0x204 the axis time (3).
    pub axis_time: f32,
    /// +0x208 the roll of the axis turn.
    pub axis_z: f32,
    /// +0x20c the afterburner during the lag turn.
    pub afterburner_20c: u8,
    /// +0x210 the turn throttle (1).
    pub scale_210: f32,
    /// +0x214 the axis pause is armed.
    pub axis_armed: u8,
    /// +0x218 the in-sights clock.
    pub sight_timer: f32,
    /// +0x21c the in-sights time (4).
    pub sight_time: f32,
    /// +0x220 the trail extent.
    pub trail_extent: f32,
    /// +0x22c the player steers.
    pub direction_set: u8,
    /// +0x230..+0x238 the player's steering.
    pub direction: Vector,
    /// +0x23c the match-throttle idea drives.
    pub match_driving: u8,
    /// +0x244 the catch-up range (1500).
    pub catch_up_range: f32,
}

impl TrailBehavior {
    /// `TrailBehavior::TrailBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            match_throttle: MatchThrottle2Idea::new(),
            face: FaceIdea::new(),
            move_idea: MoveIdea::new(),
            afterburner: AfterburnerIdea::default(),
            directive_target: None,
            leader_1dc: 0,
            has_directive_target: 0,
            trail_offset: 0.0,
            trail_distance: 50.0,
            lagging: 0,
            facing_cos: r(0.523_598_790_168_762_2_f64.cos()),
            lag_timer: 0.0,
            axis_timer: 0.0,
            lag_time: 2.5,
            axis_pause: 5.0,
            axis_time: 3.0,
            axis_z: 0.0,
            afterburner_20c: 0,
            scale_210: 1.0,
            axis_armed: 0,
            sight_timer: 0.0,
            sight_time: 4.0,
            trail_extent: 0.0,
            direction_set: 0,
            direction: Vector::ZERO,
            match_driving: 0,
            catch_up_range: 1500.0,
        }
    }

    /// Slot 3's body (the leader changes in slot 7 run it too).
    fn restart<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        cx.set_cruise_lock(1, 3);
        let leader = cx
            .target_view()
            .or_else(|| cx.view_of(self.directive_target));
        self.lagging = 0;
        self.lag_timer = 0.0;
        self.axis_timer = 0.0;
        if let Some(own) = cx.own()
            && own.is_player
        {
            self.match_throttle.flag_38 = u8::from(own.cruise.is_some_and(|c| c.0));
        }
        self.direction_set = 0;
        self.direction = Vector::ZERO;
        self.sight_timer = 0.0;
        if let Some(l) = leader {
            publish_target_id(cx, l.id);
            publish_target_id(cx, l.id);
            self.leader_1dc = l.id;
        } else {
            publish_target_id(cx, 0);
            self.leader_1dc = 0;
        }
        let p = cx.state.personality.trail;
        self.facing_cos = r(e(p.lock_cone_angle).cos());
        self.lag_time = p.break_time;
        self.axis_time = p.min_no_lock_time;
        self.axis_z = p.break_roll_throttle;
        self.afterburner_20c = p.break_afterburner;
        self.scale_210 = p.max_turn_throttle;
        self.face.set_scale_94(p.max_turn_throttle);
        self.axis_armed = 0;
        self.match_driving = 0;
    }

    /// The axis turn of the lag and axis pauses.
    fn face_axis(&mut self) {
        self.face.set_axis(Vector::new(0.0, 0.0, self.axis_z));
    }

    /// The AI's own pauses before the trail proper: `Some(result)` when one
    /// of them took the tick.
    fn pauses<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        leader: &ObjectView,
    ) -> Option<i32> {
        let dt = self.base.tick_delta;
        let to_leader = normalize3(sub(leader.center, own.center));
        let m = &own.orientation.m;
        let forward = Vector::new(-m[0][2], -m[1][2], -m[2][2]);
        let facing = r(clamped_dot3(to_leader, forward));
        if self.axis_armed != 0
            && self.lagging == 0
            && e(self.axis_pause) + e(self.axis_time) > e(self.axis_timer)
        {
            if self.axis_timer > self.axis_time {
                self.axis_timer = r(e(self.axis_timer) + e(dt));
                self.axis_armed = 0;
                self.face_axis();
                self.face.update(cx);
                return Some(1);
            }
            self.axis_timer = r(e(dt) + e(self.axis_timer));
        } else {
            self.axis_timer = 0.0;
        }
        if self.lagging != 0 && facing < self.facing_cos && self.lag_timer < self.lag_time {
            self.lag_timer = r(e(dt) + e(self.lag_timer));
            self.face_axis();
            if self.afterburner_20c != 0 {
                Idea::<W>::update(&mut self.afterburner, cx, dt);
            }
            self.face.update(cx);
            self.move_idea.set_mode(cx, 0);
            self.move_idea.set_scale_40(1.0);
            let orientation = cx.own().map_or(own.orientation, |v| v.orientation);
            self.move_idea.set_direction_from(&orientation);
            self.move_idea.set_angle_38(PI);
            self.move_idea.update(cx, dt);
            return Some(0);
        }
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.lag_timer = 0.0;
        if facing > self.facing_cos {
            self.lagging = 1;
            self.axis_armed = 1;
        } else {
            self.lagging = 0;
        }
        if self.lagging != 0 {
            let was_below = self.sight_timer < self.sight_time;
            self.sight_timer = r(e(dt) + e(self.sight_timer));
            if self.sight_timer > self.sight_time && was_below && cx.state.content_callback {
                cx.state.sys.events.push(CombatEvent::InSights(leader.id));
            }
        } else {
            self.sight_timer = 0.0;
        }
        None
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for TrailBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        if !self.base.bind(owner_id) {
            return false;
        }
        self.has_directive_target = 0;
        self.match_throttle.max_bearing_34 = PI;
        self.match_throttle.catch_up_range_58 = self.catch_up_range;
        Idea::<W>::bind(&mut self.afterburner, owner_id);
        true
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.face.set_context(&*cx);
        self.match_throttle.set_context(&*cx);
        if !cx.own().is_some_and(|v| v.is_player) {
            self.match_throttle.flag_3b = 1;
        }
        self.move_idea.set_context(&*cx);
        self.move_idea.set_mode(cx, 1);
    }

    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.restart(cx);
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.has_directive_target = 0;
        self.directive_target = None;
        self.face.commit(cx);
        self.match_throttle.flag_39 = 1;
        self.match_throttle.commit(cx);
        self.move_idea.commit(cx);
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.base.disengage(cx.state);
    }

    /// Slot 5: 1 behind a moving ship-like target (2 when the target has
    /// the ship within its rear half or is a large ship), or with a
    /// directive target.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = 2;
        let own = cx.own();
        let is_player = own.as_ref().is_some_and(|v| v.is_player);
        let behavior = cx.state.behavior;
        let personality = cx.state.personality;
        let gate = cx
            .state
            .sys
            .formation
            .gate_a(&*cx.world, is_player, behavior, &personality);
        if !gate {
            if let (Some(leader), Some(own)) = (cx.target_view(), own.as_ref())
                && iface_vector_magnitude(Some(&leader)) >= 20.0
            {
                if leader.type_flags & 0x38_0000 != 0 {
                    return self.answer(result);
                }
                if own.type_flags & 0x7f_0000 != 0 {
                    result = 1;
                }
                let bearing =
                    bearing_angle_to_object(leader.center, &leader.orientation, own.center);
                if bearing < e(HALF_PI) {
                    return self.answer(2);
                }
            }
            if self.has_directive_target != 0 {
                result = 1;
            }
        }
        self.answer(result)
    }

    fn evaluate(&mut self, _cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        // `PlayerTrailAvailable` is false in the server build.
        EvaluateResult::Skip
    }

    /// Slot 8: the match-throttle idea's cruise follow while it drives.
    fn end_tick(&mut self, cx: &mut Cx<'_, W>) {
        if self.match_driving != 0 {
            self.match_throttle.slot2(cx);
        }
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let Some(own) = cx.own().filter(|_| self.base.bound) else {
            self.base.result = 3;
            return 3;
        };
        if cx.timer_active(slot::EVADE_BREAK) {
            self.base.result = 2;
            return 2;
        }
        let dt = self.base.tick_delta;
        let mut from_target = false;
        let leader = if self.has_directive_target != 0
            && let Some(d) = cx.view_of(self.directive_target)
        {
            d
        } else {
            let Some(t) = cx.target_view() else {
                self.base.result = 2;
                return 2;
            };
            if !own.is_player && iface_vector_magnitude(Some(&t)) < 20.0 {
                self.base.result = 3;
                return 3;
            }
            from_target = true;
            t
        };
        publish_target_id(cx, leader.id);
        publish_target_id(cx, leader.id);
        publish_target_id(cx, leader.id);
        if leader.id != self.leader_1dc {
            self.restart(cx);
        }
        self.leader_1dc = leader.id;
        if from_target
            && !own.is_player
            && let Some(result) = self.pauses(cx, &own, &leader)
        {
            self.base.result = result;
            return result;
        }

        if !own.is_player {
            self.trail_distance = cx.state.personality.trail.distance;
        }
        self.trail_offset = r(e(max_scalar_attribute(&leader)) + e(self.trail_distance));
        self.trail_extent = r(e(max_scalar_attribute(&leader)) + e(self.trail_offset));
        let offset = transform_matrix(
            &leader.orientation,
            Vector::new(0.0, 0.0, self.trail_offset),
        );
        let c = leader.center;
        let point = Vector::new(
            r(e(offset.x) + e(c.x)),
            r(e(offset.y) + e(c.y)),
            r(e(offset.z) + e(c.z)),
        );
        self.match_driving = u8::from(is_aligned_and_outside_heading_cone(
            &own,
            Some(&leader),
            0.0,
        ));
        if self.match_driving != 0 {
            self.match_throttle
                .set_watch_and_position(cx, Some(leader.id), point);
            let timer4 = cx.timer_active(slot::AFTERBURNER);
            self.match_throttle.update(cx, timer4);
        } else {
            self.move_idea.set_mode(cx, 0);
            self.move_idea.set_scale_40(1.0);
            self.move_idea.set_watch_object(cx, Some(leader.id));
            self.move_idea.set_angle_38(PI);
            self.move_idea.set_range_3c(self.trail_distance);
            self.move_idea.update(cx, dt);
        }
        // `GetInterceptClearanceOfIface` for effect.
        let _ = get_intercept_clearance_of_iface(&own, &leader);
        self.face.set_watch_object(Some(leader.id));
        if cx.timer_active(slot::ROLL) {
            self.face.set_up_vector(Vector::new(0.0, 0.0, 1.0));
        }
        self.face.update(cx);
        self.base.result = 1;
        1
    }

    /// Slot 10: the op's trail distance and object.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let id = op_dword(op, 0x0c);
        let distance = f32::from_bits(op_dword(op, 0x10));
        if !matches!(op, DirectiveOp::Other(_)) {
            self.trail_distance = distance;
        }
        if let Some(t) = cx.view_of((id != 0).then_some(id)) {
            self.directive_target = Some(t.id);
            self.has_directive_target = 1;
            self.base.flags |= 0x2;
            return true;
        }
        self.base.flags &= 0xfd;
        false
    }

    fn set_direction(&mut self, cx: &mut Cx<'_, W>, dir: Vector) {
        self.direction_set = 1;
        let mut x = -dir.y;
        let mut y = -dir.x;
        let factor = cx.state.turn_sensitivity;
        if factor > 0.0 {
            let rescale = 1.0 / e(factor);
            x = r(rescale * e(x));
            y = r(rescale * e(y));
        }
        let clamp = |v: f32| {
            if !(v > -1.0) {
                -1.0
            } else if !(v < 1.0) {
                1.0
            } else {
                v
            }
        };
        self.direction = Vector::new(clamp(x), clamp(y), 0.0);
        if dir.z > 0.0 {
            self.trail_distance = dir.z;
        }
    }

    fn set_cruise(&mut self, _cx: &mut Cx<'_, W>, on: bool) {
        self.match_throttle.flag_38 = u8::from(on);
    }
}

impl TrailBehavior {
    /// The poll's answer, 3 while `+0x29` is set.
    fn answer(&mut self, mode: i32) -> i32 {
        self.base.mode = if self.base.flag29 != 0 { 3 } else { mode };
        self.base.mode
    }
}
