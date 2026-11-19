//! `BuzzBehavior` (behaviour 1, `Common/common_buzzbehavior.cpp`, class in
//! `include/common_buzzbehavior.h`): the attack run. Head away from the
//! target (state 0) until it is far enough, head toward it in one of
//! three styles (state 1: straight, slide, waggle, each a dodge of the
//! embedded dodge idea or a lock on the target), then pass it in one of
//! three styles (state 2: break away, straight by, engine kill), and
//! start over. A fast path takes over while the ship sits behind its
//! target heading the same way.
//!
//! The reference reads the target and the ship through `FAiTarget`
//! records; the port reads their views ([`super::intercept`]).

use neuron_core::math::{Matrix, Vector};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx};
use super::dodge::{BreakAwayIdea, DodgeIdea};
use super::face_idea::FaceIdea;
use super::fp::{column2, e, neg, r, sub};
use super::idea::{AfterburnerIdea, Idea, publish_target_id};
use super::intercept::{
    get_intercept_clearance, is_aligned_and_outside_heading_cone, is_target_co_aligned_and_ahead,
    is_within_intercept_range,
};
use super::move_idea::MoveIdea;
use super::steer::random_vector;
use super::timer_table::slot;
use super::world::ObjectView;

/// `1 / 32768`.
const RAND_SCALE: f64 = 1.0 / 32768.0;
/// π/8 (`0x3ec90fdb`).
const EIGHTH_PI: f32 = core::f32::consts::FRAC_PI_8;
/// π/16 (`0x3e490fdb`).
const SIXTEENTH_PI: f32 = 0.196_349_55;
/// π/2 (`0x3fc90fdb`).
const HALF_PI: f32 = core::f32::consts::FRAC_PI_2;

/// `AngleToBasisZColumn3(basis, a, b)` (`include/common_ini.h`): the angle
/// between `a - b` (each component rounded) and the basis's column 2, its
/// cosine rounded and clamped; unrounded.
#[must_use]
pub fn angle_to_basis_z_column(basis: &Matrix, a: Vector, b: Vector) -> f64 {
    let d = sub(a, b);
    let len = ((e(d.z) * e(d.z) + e(d.x) * e(d.x)) + e(d.y) * e(d.y)).sqrt();
    let (mut nx, mut ny, mut nz) = (0.0, 0.0, 0.0);
    if len > e(0.01) {
        let inv = e(r(1.0 / len));
        nx = inv * e(d.x);
        ny = e(d.y) * inv;
        nz = e(d.z) * inv;
    }
    let c = column2(basis);
    // `f32::clamp` passes NaN, as the two compares do.
    let dot = r((nz * e(c.z) + ny * e(c.y)) + nx * e(c.x)).clamp(-1.0, 1.0);
    e(dot).acos().abs()
}

/// A weighted pick of three styles: `rand() * (a + b + c) / 32768`
/// below `first` gives 1, below `second + first` 2, else 0.
fn pick_style<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, first: f32, second: f32, third: f32) -> i32 {
    let sum = r((e(first) + e(second)) + e(third));
    let pick = f64::from(cx.rand()) * e(sum) * RAND_SCALE;
    if pick < e(first) {
        1
    } else if pick < e(second) + e(first) {
        2
    } else {
        0
    }
}

/// `BuzzBehavior` (0x354 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct BuzzBehavior {
    base: BehaviorBase,
    /// +0x2c `WatcherA`, the move idea.
    pub move_idea: MoveIdea,
    /// +0xa4 `WatcherB`, the face idea.
    pub face: FaceIdea,
    /// +0x164 `WatcherC`, the afterburner idea.
    pub afterburner: AfterburnerIdea,
    /// +0x178 `WatcherD`, the dodge idea.
    pub dodge: DodgeIdea,
    /// +0x258 `WatcherE`, the break-away idea.
    pub break_away: BreakAwayIdea,
    /// +0x2f4 a random unit vector drawn on engage, never read.
    pub field_2f4: Vector,
    /// +0x300 (300 from `SetContext`).
    pub field_300: f32,
    /// +0x304.
    pub field_304: u8,
    /// +0x308 the fast path's clock.
    pub field_308: f32,
    /// +0x30c its limit (5).
    pub field_30c: f32,
    /// +0x310 the time the fast path has been facing.
    pub field_310: f32,
    /// +0x314 the facing time before the afterburner (1.5).
    pub field_314: f32,
    /// +0x318 0 head away, 1 head toward, 2 pass by.
    pub state: i32,
    /// +0x31c 0 straight to, 1 slide, 2 waggle.
    pub head_toward_style: i32,
    /// +0x320 0 break away, 1 straight by, 2 engine kill.
    pub pass_by_style: i32,
    /// +0x324 `MinDistanceToHeadToward` (500).
    pub min_distance_to_head_toward: f32,
    /// +0x32c `DistanceToPassBy` (100).
    pub distance_to_pass_by: f32,
    /// +0x334 `PassByTime` (3).
    pub pass_by_time: f32,
    /// +0x338 `PassByElapsed`.
    pub pass_by_elapsed: f32,
    /// +0x33c `MaxTimeToHeadAway` (6).
    pub max_time_to_head_away: f32,
    /// +0x340 `HeadAwayElapsed`.
    pub head_away_elapsed: f32,
    /// +0x344 the fast path's clearance limit (300 on engage).
    pub field_344: f32,
    /// +0x348 `PassByRollThrottle`.
    pub pass_by_roll_throttle: f32,
    /// +0x34c `HeadTowardEngineThrottle` (1).
    pub head_toward_engine_throttle: f32,
    /// +0x350 `HeadTowardTurnThrottle` (1).
    pub head_toward_turn_throttle: f32,
}

impl BuzzBehavior {
    /// `BuzzBehavior::BuzzBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            afterburner: AfterburnerIdea::default(),
            dodge: DodgeIdea::new(),
            break_away: BreakAwayIdea::new(),
            field_2f4: Vector::ZERO,
            field_300: 0.0,
            field_304: 0,
            field_308: 0.0,
            field_30c: 0.0,
            field_310: 0.0,
            field_314: 1.5,
            state: 0,
            head_toward_style: 0,
            pass_by_style: 0,
            min_distance_to_head_toward: 500.0,
            distance_to_pass_by: 100.0,
            pass_by_time: 3.0,
            pass_by_elapsed: 0.0,
            max_time_to_head_away: 6.0,
            head_away_elapsed: 0.0,
            field_344: 0.0,
            pass_by_roll_throttle: 0.0,
            head_toward_engine_throttle: 1.0,
            head_toward_turn_throttle: 1.0,
        }
    }

    /// `ApplySteeringFate(fate, interval, variance)`: the dodge idea's
    /// style and interval, then its tick.
    fn apply_steering_fate<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        fate: i32,
        interval: f32,
        variance: f32,
    ) {
        self.dodge.dodge_style = fate;
        self.dodge.interval_time = interval;
        self.dodge.interval_time_variance_percent = variance;
        let dt = self.base.tick_delta;
        self.dodge.update(cx, dt);
    }

    /// `AcquireLockEffects`: face the target (with the roll timer's up
    /// vector) and fly along the ship's heading, with the afterburner
    /// while the ship is behind it.
    fn acquire_lock_effects<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        target: &ObjectView,
    ) {
        let dt = self.base.tick_delta;
        publish_target_id(cx, target.id);
        self.face.set_scale_94(self.head_toward_turn_throttle);
        self.face.set_watch_object(Some(target.id));
        if cx.timer_active(slot::ROLL) {
            self.face.set_up_vector(Vector::new(0.0, 0.0, 1.0));
        }
        self.face.update(cx);
        let scale = if is_aligned_and_outside_heading_cone(own, Some(target), 0.0) {
            Idea::<W>::update(&mut self.afterburner, cx, dt);
            1.0
        } else {
            Idea::<W>::commit(&mut self.afterburner, cx);
            self.head_toward_engine_throttle
        };
        self.move_idea.set_scale_40(scale);
        publish_target_id(cx, target.id);
        self.move_idea.set_direction(neg(column2(&own.orientation)));
        self.move_idea.update(cx, dt);
    }

    /// The fast path: behind the target and heading its way, within the
    /// clearance limit, match its frame. `None` when it does not apply.
    fn fast_path<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        target: &ObjectView,
    ) -> Option<i32> {
        if self.state == 2
            || !is_target_co_aligned_and_ahead(own, target, 0.0)
            || !(get_intercept_clearance(own, target) < e(self.field_344))
        {
            return None;
        }
        let dt = self.base.tick_delta;
        let basis = target.orientation;
        self.face.set_basis(&basis);
        self.face.set_angle_weight_8c(EIGHTH_PI);
        self.face.set_scale_94(1.0);
        if self.face.update(cx) == 1 {
            if self.field_310 > self.field_314 {
                Idea::<W>::update(&mut self.afterburner, cx, dt);
            }
            self.field_310 = r(e(self.field_310) + e(dt));
        } else {
            self.field_310 = 0.0;
        }
        self.move_idea.set_direction_from(&basis);
        self.move_idea.set_scale_40(1.0);
        self.move_idea.update(cx, dt);
        let sum = e(self.field_308) + e(dt);
        self.field_308 = r(sum);
        if sum > e(self.field_30c) {
            Some(0)
        } else {
            self.field_308 = 0.0;
            Some(1)
        }
    }

    /// State 0's head-away styles while the target is within the head
    /// toward distance.
    fn head_away<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        target: &ObjectView,
    ) {
        let dt = self.base.tick_delta;
        let head = cx.state.personality.buzz_head_toward;
        match self.head_toward_style {
            0 => {
                Idea::<W>::update(&mut self.afterburner, cx, dt);
                if head.roll_throttle != 0.0 {
                    self.apply_steering_fate(
                        cx,
                        3,
                        head.dodge_interval_time,
                        head.dodge_interval_time_variance_percent,
                    );
                } else {
                    self.move_idea.set_scale_40(1.0);
                    self.move_idea.set_direction(neg(column2(&own.orientation)));
                    self.move_idea.update(cx, dt);
                    self.face.set_direction(sub(own.center, target.center));
                    self.face.set_scale_94(1.0);
                    if cx.timer_active(slot::ROLL) {
                        self.face.set_up_vector(Vector::new(0.0, 0.0, 1.0));
                    }
                    self.face.update(cx);
                }
            }
            1 => {
                Idea::<W>::update(&mut self.afterburner, cx, dt);
                self.apply_steering_fate(
                    cx,
                    2,
                    head.slide_interval_time,
                    head.slide_interval_time_variance_percent,
                );
            }
            2 => {
                Idea::<W>::update(&mut self.afterburner, cx, dt);
                self.apply_steering_fate(
                    cx,
                    0,
                    head.dodge_interval_time,
                    head.dodge_interval_time_variance_percent,
                );
            }
            _ => {}
        }
    }

    /// State 1's head-toward styles within the head toward distance.
    fn head_toward<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        target: &ObjectView,
    ) {
        let dt = self.base.tick_delta;
        let head = cx.state.personality.buzz_head_toward;
        let angle = angle_to_basis_z_column(&own.orientation, own.center, target.center);
        match self.head_toward_style {
            0 => {
                if angle < e(EIGHTH_PI) && head.roll_throttle != 0.0 {
                    self.apply_steering_fate(
                        cx,
                        3,
                        head.dodge_interval_time,
                        head.dodge_interval_time_variance_percent,
                    );
                } else {
                    self.acquire_lock_effects(cx, own, target);
                }
            }
            1 => {
                if angle < e(SIXTEENTH_PI) {
                    self.apply_steering_fate(
                        cx,
                        2,
                        head.slide_interval_time,
                        head.slide_interval_time_variance_percent,
                    );
                } else {
                    self.acquire_lock_effects(cx, own, target);
                }
            }
            2 => {
                if angle < e(HALF_PI) {
                    self.dodge.dodge_style = 0;
                    self.dodge.interval_time = head.dodge_interval_time;
                    self.dodge.interval_time_variance_percent =
                        head.dodge_interval_time_variance_percent;
                    self.dodge.set_field_58(sub(target.center, own.center));
                    self.dodge.update(cx, dt);
                } else {
                    self.acquire_lock_effects(cx, own, target);
                }
            }
            _ => {}
        }
    }

    /// State 2's pass-by styles while the pass-by time runs.
    fn pass_by<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        own: &ObjectView,
        target: &ObjectView,
    ) {
        let dt = self.base.tick_delta;
        match self.pass_by_style {
            0 => {
                self.break_away.turn_throttle =
                    cx.state.personality.buzz_pass_by.break_turn_throttle;
                self.break_away.break_style = 1;
                self.break_away.set_watch_object(Some(target.id));
                self.break_away.update(cx, dt);
            }
            1 => {
                publish_target_id(cx, 0);
                self.face
                    .set_axis(Vector::new(0.0, 0.0, self.pass_by_roll_throttle));
                self.face.update(cx);
                publish_target_id(cx, 0);
                self.move_idea.set_scale_40(1.0);
                self.move_idea.set_direction(neg(column2(&own.orientation)));
                self.move_idea.update(cx, dt);
            }
            2 => {
                cx.set_engine_active(false);
                self.face.set_watch_object(Some(target.id));
                self.face.set_scale_94(1.0);
                self.face.update(cx);
            }
            _ => {}
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for BuzzBehavior {
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
        self.field_300 = 300.0;
        self.field_304 = 0;
        self.move_idea.set_field_44(2);
        self.move_idea.set_angle_38(core::f32::consts::PI);
        self.move_idea.set_range_3c(0.0);
        Idea::<W>::bind(&mut self.afterburner, owner_id);
        Idea::<W>::bind(&mut self.dodge, owner_id);
        Idea::<W>::bind(&mut self.break_away, owner_id);
        true
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.move_idea.set_context(&*cx);
        // `SetMode(0)` on a fresh move idea (mode 0) commands nothing.
        self.move_idea.set_mode(cx, 0);
        self.face.set_context(&*cx);
    }

    /// `RecomputeApproach` (slot 3).
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        cx.set_cruise_lock(1, 3);
        self.field_344 = 300.0;
        self.field_308 = 0.0;
        self.field_304 = 0;
        self.field_2f4 = random_vector(&mut cx.world, 1.0);
        self.field_30c = 5.0;
        self.field_310 = 0.0;
        self.state = 0;
        self.pass_by_elapsed = 0.0;
        self.head_away_elapsed = 0.0;

        let head = cx.state.personality.buzz_head_toward;
        let pass = cx.state.personality.buzz_pass_by;
        self.head_toward_style = pick_style(
            cx,
            head.style_weight_slide,
            head.style_weight_waggle,
            head.style_weight_straight_to,
        );
        self.pass_by_style = pick_style(
            cx,
            pass.style_weight_straight_by,
            pass.style_weight_engine_kill,
            pass.style_weight_break_away,
        );
        self.min_distance_to_head_toward = head.min_distance_to_head_toward;
        let draw = f64::from(cx.rand()) * RAND_SCALE;
        self.min_distance_to_head_toward = r(((draw + draw - 1.0)
            * e(head.min_distance_to_head_toward_variance_percent)
            + 1.0)
            * e(self.min_distance_to_head_toward));
        self.max_time_to_head_away = head.max_time_to_head_away;
        self.distance_to_pass_by = pass.distance_to_pass_by;
        self.pass_by_time = pass.time;
        self.head_toward_engine_throttle = head.engine_throttle;
        self.head_toward_turn_throttle = head.turn_throttle;

        let d = &mut self.dodge;
        d.throttle = head.engine_throttle;
        d.cone_angle = head.dodge_cone_angle;
        d.cone_angle_variance_percent = head.dodge_cone_angle_variance_percent;
        d.waggle_axis_cone_angle = head.dodge_waggle_axis_cone_angle;
        d.roll_angle = head.dodge_roll_angle;
        d.turn_throttle = head.dodge_turn_throttle;
        d.picker.set_weights(
            head.dodge_direction_weight_left,
            head.dodge_direction_weight_right,
            head.dodge_direction_weight_up,
            head.dodge_direction_weight_down,
        );
        d.vector_cc.z = head.roll_throttle;
        d.roll_flip_direction = head.roll_flip_direction;
        d.slide_throttle = head.slide_throttle;
        self.break_away.picker.set_weights(
            pass.break_direction_weight_left,
            pass.break_direction_weight_right,
            pass.break_direction_weight_up,
            pass.break_direction_weight_down,
        );
        self.pass_by_roll_throttle = pass.roll_throttle;
    }

    /// `Disengage` (slot 4): every idea commits, face first.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.face.commit(cx);
        self.move_idea.commit(cx);
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.dodge.commit(cx);
        self.break_away.commit(cx);
        self.base.disengage(cx.state);
    }

    /// `EvaluateReadiness` (slot 5): out of the head-away time, 2;
    /// otherwise 1 with a ship-like target, unless the formation block
    /// holds the ship.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut mode = 2;
        if self.head_away_elapsed >= self.max_time_to_head_away {
            self.head_away_elapsed = 0.0;
        } else {
            let is_player = cx.own().is_some_and(|v| v.is_player);
            let behavior = cx.state.behavior;
            let personality = cx.state.personality;
            let gate = cx
                .state
                .sys
                .formation
                .gate_a(&*cx.world, is_player, behavior, &personality);
            if !gate
                && cx
                    .target_view()
                    .is_some_and(|t| t.type_flags & 0x3_0000 != 0)
            {
                mode = 1;
            }
        }
        if self.base.flag29 != 0 {
            self.base.mode = 3;
            return 3;
        }
        self.base.mode = mode;
        mode
    }

    #[allow(clippy::too_many_lines)]
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let target = cx.target_view();
        let own = cx.own();
        let (Some(own), Some(target), true) = (own, target, self.base.bound) else {
            self.base.result = 3;
            return 3;
        };
        let dt = self.base.tick_delta;
        let mut result = 1;
        if let Some(r) = self.fast_path(cx, &own, &target) {
            result = r;
        } else {
            if self.state == 0 {
                if is_within_intercept_range(&own, &target, self.min_distance_to_head_toward) {
                    self.head_away(cx, &own, &target);
                    result = 1;
                    let sum = e(dt) + e(self.head_away_elapsed);
                    self.head_away_elapsed = r(sum);
                    if !(sum < e(self.max_time_to_head_away)) {
                        result = 2;
                    }
                } else {
                    Idea::<W>::commit(&mut self.afterburner, cx);
                    self.face.commit(cx);
                    self.move_idea.commit(cx);
                    self.state = 1;
                    let f = cx.state.personality.formation;
                    if f.break_apart_on_buzz_head_toward != 0 {
                        cx.notify_formation_leader_1(f.break_on_buzz_head_toward_time);
                    }
                    if f.regroup_on_buzz_head_toward != 0 {
                        cx.notify_formation_leader_2();
                    }
                }
            }
            if self.state == 1 {
                if is_within_intercept_range(&own, &target, self.distance_to_pass_by) {
                    self.face.commit(cx);
                    self.move_idea.commit(cx);
                    if cx.timer_active(slot::AFTERBURNER) {
                        Idea::<W>::update(&mut self.afterburner, cx, dt);
                    } else {
                        Idea::<W>::commit(&mut self.afterburner, cx);
                    }
                    self.state = 2;
                    let f = cx.state.personality.formation;
                    if f.break_apart_on_buzz_pass_by != 0 {
                        cx.notify_formation_leader_1(f.break_on_buzz_pass_by_time);
                    }
                    if f.regroup_on_buzz_pass_by != 0 {
                        cx.notify_formation_leader_2();
                    }
                    if cx.state.personality.buzz_pass_by.drop_bomb_on_pass_by != 0 {
                        cx.state.sys.guns.drop_all_mines(&mut *cx.world, cx.ship);
                    }
                } else {
                    if is_within_intercept_range(&own, &target, self.min_distance_to_head_toward) {
                        self.head_toward(cx, &own, &target);
                    } else {
                        let head = cx.state.personality.buzz_head_toward;
                        self.head_toward_engine_throttle = 1.0;
                        self.head_toward_turn_throttle = 1.0;
                        self.acquire_lock_effects(cx, &own, &target);
                        self.head_toward_engine_throttle = head.engine_throttle;
                        self.head_toward_turn_throttle = head.turn_throttle;
                    }
                    result = 0;
                }
            }
            if self.state == 2 {
                let sum = e(self.pass_by_elapsed) + e(dt);
                self.pass_by_elapsed = r(sum);
                if sum < e(self.pass_by_time) {
                    self.pass_by(cx, &own, &target);
                    result = 0;
                } else {
                    self.pass_by_elapsed = 0.0;
                    self.head_away_elapsed = 0.0;
                    self.state = 0;
                    result = 2;
                }
            }
        }
        if cx.timer_active(slot::EVADE_BREAK) {
            result = 2;
        }
        self.base.result = result;
        result
    }

    fn target(&self) -> Option<u32> {
        None
    }
}
