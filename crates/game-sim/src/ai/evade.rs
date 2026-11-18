//! `EvadeBehavior` (behaviour 5, `Common/common_evadebehavior.cpp`, class
//! in `include/common_evadebehavior.h`): get away from the closest hostile
//! that has the ship (or one of its formation followers) in its sights.
//! Mode 1 dodges (the dodge idea) for the dodge time, mode 2 breaks away
//! (the break-away idea) for the break time, mode 3 reverses while the
//! brake-reverse timer runs, mode 4 dodges the missile the timer table
//! tracks.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp};
use super::behaviors::op_dword;
use super::dodge::{BreakAwayIdea, DodgeIdea, DodgeMissileIdea};
use super::face::FaceBehavior;
use super::face_idea::FaceIdea;
use super::fp::{column2, e, neg, r};
use super::idea::{AfterburnerIdea, Idea};
use super::intercept::{
    iface_vector_magnitude, is_point_outside_heading_cone, is_within_intercept_range_of_iface,
};
use super::timer_table::slot;
use super::world::ObjectView;

/// `1 / 32768`.
const RAND_SCALE: f64 = 1.0 / 32768.0;
/// π/2 (`0x3fc90fdb`).
const HALF_PI: f32 = core::f32::consts::FRAC_PI_2;

/// `EvadeBehavior` (0x2c0 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct EvadeBehavior {
    base: BehaviorBase,
    /// +0x2c the mode (1 to 4; 0 after engage).
    pub mode_2c: i32,
    /// +0x30 the dodge idea.
    pub dodge: DodgeIdea,
    /// +0x110 the break-away idea.
    pub break_away: BreakAwayIdea,
    /// +0x1ac the missile-dodge idea.
    pub dodge_missile: DodgeMissileIdea,
    /// +0x1cc a face idea (bound, never ticked).
    pub face: FaceIdea,
    /// +0x28c the afterburner idea.
    pub afterburner: AfterburnerIdea,
    /// +0x2a0 the object being evaded.
    pub watched: Option<u32>,
    /// +0x2a8 the dodge time (10 from `SetContext`).
    pub dodge_time: f32,
    /// +0x2ac the break time (3).
    pub break_time: f32,
    /// +0x2b0 the time in the behaviour.
    pub elapsed: f32,
    /// +0x2b4 the evade-break timer's latch.
    pub flag_2b4: u8,
    /// +0x2b5 the dodge-more timer's latch.
    pub flag_2b5: u8,
    /// +0x2b6 never written but by the constructor.
    pub flag_2b6: u8,
    /// +0x2b7 never written but by the constructor.
    pub flag_2b7: u8,
    /// +0x2b8 the dodge mode's narrow cone (`-cos(π/3)` for a slide).
    pub cone_2b8: f32,
    /// +0x2bc the threat cone (`0x3f34fdf4`).
    pub cone_2bc: f32,
}

impl EvadeBehavior {
    /// `EvadeBehavior::EvadeBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            mode_2c: 0,
            dodge: DodgeIdea::new(),
            break_away: BreakAwayIdea::new(),
            dodge_missile: DodgeMissileIdea::default(),
            face: FaceIdea::new(),
            afterburner: AfterburnerIdea::default(),
            watched: None,
            dodge_time: 0.0,
            break_time: 3.0,
            elapsed: 0.0,
            flag_2b4: 0,
            flag_2b5: 0,
            flag_2b6: 0,
            flag_2b7: 0,
            cone_2b8: 0.0,
            cone_2bc: f32::from_bits(0x3f34_fdf4),
        }
    }

    /// The watched object while it exists.
    fn watched_view<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> Option<ObjectView> {
        cx.view_of(self.watched)
    }

    /// `WatchedOutsideCone`: the watched object's position outside the
    /// ship's heading cone of cosine `cos`.
    fn outside_cone(watched: &ObjectView, own: &ObjectView, cos: f32) -> bool {
        is_point_outside_heading_cone(watched.position, Some(own), cos)
    }

    /// `PickDodgeStyle`: one draw over the four style weights.
    fn pick_dodge_style<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>) -> i32 {
        let p = cx.state.personality.evade_dodge;
        let sum = r(
            ((e(p.style_weight_waggle) + e(p.style_weight_waggle_random))
                + e(p.style_weight_slide))
                + e(p.style_weight_corkscrew),
        );
        let pick = f64::from(cx.rand()) * e(sum) * RAND_SCALE;
        let mut bound = e(p.style_weight_waggle);
        if pick < bound {
            return 0;
        }
        bound += e(p.style_weight_waggle_random);
        if pick < bound {
            return 1;
        }
        bound += e(p.style_weight_slide);
        if pick < bound {
            return 2;
        }
        3
    }

    /// Slot 4's body, which slot 7 also calls on a mode change.
    fn commit_all<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.flag_2b5 = 0;
        self.dodge.commit(cx);
        self.break_away.commit(cx);
        self.dodge_missile.commit(cx);
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.base.disengage(cx.state);
    }

    /// The poll's answer, 3 while `+0x29` is set.
    fn answer(&mut self, mode: i32) -> i32 {
        self.base.mode = if self.base.flag29 != 0 { 3 } else { mode };
        self.base.mode
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for EvadeBehavior {
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
        Idea::<W>::bind(&mut self.dodge, owner_id);
        self.dodge.cone_angle = HALF_PI;
        self.dodge.dodge_distance = 50.0;
        self.dodge.interval_time = 6.0;
        Idea::<W>::bind(&mut self.break_away, owner_id);
        Idea::<W>::bind(&mut self.dodge_missile, owner_id);
        Idea::<W>::bind(&mut self.afterburner, owner_id);
        self.dodge_time = 10.0;
        self.elapsed = 0.0;
        true
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if self.base.bound {
            self.face.set_context(&*cx);
        }
    }

    /// Slot 3: the dodge and break parameters from the personality, the
    /// dodge style and the break style drawn, the afterburner delay
    /// jittered.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.mode_2c = 0;
        cx.set_cruise_lock(1, 3);
        self.elapsed = 0.0;
        let watched = self.watched_view(cx).map(|v| v.id);
        self.dodge.picker.set_watch_object(watched);
        self.break_away.picker.set_watch_object(watched);
        let own = cx.own();
        let is_player = own.as_ref().is_some_and(|v| v.is_player);
        if is_player {
            let throttle = own.as_ref().and_then(|v| v.throttle).unwrap_or(1.0);
            self.dodge.throttle = throttle;
            self.break_away.throttle = throttle;
        }
        let d = cx.state.personality.evade_dodge;
        let b = cx.state.personality.evade_break;
        self.dodge.cone_angle = d.cone_angle;
        self.dodge.cone_angle_variance_percent = d.cone_angle_variance_percent;
        self.dodge.waggle_axis_cone_angle = d.waggle_axis_cone_angle;
        self.dodge.roll_angle = d.roll_angle;
        self.dodge.interval_time = d.interval_time;
        self.dodge.interval_time_variance_percent = d.interval_time_variance_percent;
        self.dodge.dodge_distance = d.distance;
        self.dodge_time = d.time;
        self.break_time = b.time;
        self.dodge.turn_throttle = d.turn_throttle;
        self.dodge.slide_throttle = d.slide_throttle;
        self.dodge.vector_cc.x = d.corkscrew_turn_throttle;
        self.dodge.vector_cc.z = d.corkscrew_roll_throttle;
        self.dodge.roll_flip_direction = d.corkscrew_roll_flip_direction;
        self.dodge.picker.set_weights(
            d.direction_weight_left,
            d.direction_weight_right,
            d.direction_weight_up,
            d.direction_weight_down,
        );
        let style = Self::pick_dodge_style(cx);
        self.dodge.dodge_style = style;
        if style == 2 {
            self.cone_2b8 = -r(1.047_197_580_337_524_4_f64.cos());
        }
        let sum =
            r((e(b.style_weight_outrun) + e(b.style_weight_sideways)) + e(b.style_weight_reverse));
        let pick = f64::from(cx.rand()) * e(sum) * RAND_SCALE;
        self.break_away.break_style = if pick < e(b.style_weight_reverse) {
            2
        } else if pick < e(b.style_weight_outrun) + e(b.style_weight_reverse) {
            0
        } else {
            1
        };
        self.break_away.attempt_reverse_time = b.attempt_reverse_time;
        self.break_away.reverse_distance = b.reverse_distance;
        self.break_away.turn_throttle = b.turn_throttle;
        self.break_away.roll_throttle = b.roll_throttle;
        self.break_away.interval_time = b.interval_time;
        self.break_away.picker.set_weights(
            b.direction_weight_left,
            b.direction_weight_right,
            b.direction_weight_up,
            b.direction_weight_down,
        );
        let spread = r(e(b.afterburner_delay) * e(b.afterburner_delay_variance_percent));
        let draw = f64::from(cx.rand()) * e(spread) * RAND_SCALE;
        self.break_away.afterburner_delay = r(e(b.afterburner_delay) - draw);
        if is_player && let Some(v) = own {
            self.dodge.set_field_58(neg(column2(&v.orientation)));
        }
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.commit_all(cx);
    }

    /// Slot 5: 0 with a missile to react to, 1 when the closest hostile
    /// within the activate range has the ship or a follower in its sights.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = 2;
        let p = cx.state.personality;
        if p.job.force_attack_formation != 0 && cx.timer_active(slot::EVADE_BREAK) {
            self.flag_2b4 = 1;
            self.elapsed = self.dodge_time;
            result = 0;
        }
        if cx.state.sys.missile_reaction(&p) {
            return self.answer(0);
        }
        let is_player = cx.own().is_some_and(|v| v.is_player);
        let behavior = cx.state.behavior;
        if cx
            .state
            .sys
            .formation
            .gate_a(&*cx.world, is_player, behavior, &p)
        {
            return self.answer(2);
        }
        self.watched = FaceBehavior::closest_hostile(cx, p.evade_dodge.evade_activate_range);
        if let (Some(w), Some(own)) = (self.watched_view(cx), cx.own())
            && let Some(victim) = w.target
        {
            if iface_vector_magnitude(Some(&w)) > 1.0
                && Self::outside_cone(&w, &own, self.cone_2bc)
                && victim == own.id
            {
                return self.answer(1);
            }
            for f in own.formation_followers.iter().take(0x10) {
                let Some(follower) = cx.view_of(Some(*f)) else {
                    continue;
                };
                if !is_point_outside_heading_cone(w.position, Some(&follower), self.cone_2bc) {
                    continue;
                }
                if victim == follower.id {
                    return self.answer(1);
                }
            }
        }
        self.answer(result)
    }

    /// Slot 8: the timer latches.
    fn end_tick(&mut self, cx: &mut Cx<'_, W>) {
        self.flag_2b4 = 0;
        if cx.state.personality.job.force_attack_formation != 0 {
            self.flag_2b4 = u8::from(cx.timer_active(slot::EVADE_BREAK));
        }
        if self.flag_2b4 != 0 {
            self.elapsed = self.dodge_time;
        }
        if self.flag_2b5 == 0 {
            self.flag_2b5 = u8::from(cx.timer_active(slot::EVADE_DODGE_MORE));
        }
    }

    #[allow(clippy::too_many_lines)]
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let Some(own) = cx.own() else {
            self.base.result = 2;
            return 2;
        };
        let dt = self.base.tick_delta;
        if !own.is_player {
            let ok = self
                .watched_view(cx)
                .is_some_and(|w| Self::outside_cone(&w, &own, self.cone_2bc));
            if !ok {
                self.base.result = 2;
                return 2;
            }
        }
        let p = cx.state.personality;
        let old = self.mode_2c;
        if cx.state.sys.missile_reaction(&p) {
            self.mode_2c = 4;
        } else if cx.timer_active(slot::BRAKE_REVERSE) {
            self.mode_2c = 3;
        } else if self.flag_2b6 == 0 && self.elapsed < self.dodge_time {
            self.mode_2c = 1;
        } else {
            if self.flag_2b7 != 0 || !(e(self.break_time) + e(self.dodge_time) > e(self.elapsed)) {
                self.base.result = 2;
                return 2;
            }
            self.mode_2c = 2;
        }
        if old != self.mode_2c {
            let f = p.formation;
            if self.mode_2c == 1 {
                if f.break_apart_on_evade_dodge != 0 {
                    cx.notify_formation_leader_1(f.break_on_evade_dodge_time);
                }
                if f.regroup_on_evade_dodge != 0 {
                    cx.notify_formation_leader_2();
                }
            }
            if self.mode_2c == 2 {
                if f.break_apart_on_evade_break != 0 {
                    cx.notify_formation_leader_1(f.break_on_evade_break_time);
                }
                if f.regroup_on_evade_break != 0 {
                    cx.notify_formation_leader_2();
                }
            }
            self.commit_all(cx);
        }
        self.elapsed = r(e(self.elapsed) + e(dt));

        let result = match self.mode_2c {
            1 => {
                let watched = self.watched_view(cx);
                if watched
                    .as_ref()
                    .is_some_and(|w| !Self::outside_cone(w, &own, self.cone_2b8))
                {
                    cx.set_axis_throttle(Vector::ZERO);
                    cx.set_nudge_direction(Vector::ZERO);
                    cx.set_throttle(1.0, 0);
                    0
                } else {
                    if self.flag_2b5 != 0 {
                        let d = p.evade_dodge;
                        let clamp = |v: f32| {
                            let angle = e(v) * 3.0;
                            if angle > e(HALF_PI) {
                                HALF_PI
                            } else {
                                r(angle)
                            }
                        };
                        self.dodge.cone_angle = clamp(d.cone_angle);
                        self.dodge.waggle_axis_cone_angle = clamp(d.waggle_axis_cone_angle);
                        let mut interval = e(d.interval_time);
                        if interval > 1.0 {
                            interval *= 0.5;
                        } else {
                            interval += interval;
                        }
                        self.dodge.interval_time = r(interval);
                        self.dodge.turn_throttle = 1.0;
                        self.dodge.slide_throttle = 1.0;
                    }
                    self.dodge.update(cx, dt);
                    let near = watched
                        .as_ref()
                        .is_some_and(|w| is_within_intercept_range_of_iface(&own, w, 50.0));
                    if near {
                        Idea::<W>::update(&mut self.afterburner, cx, dt);
                    } else {
                        Idea::<W>::commit(&mut self.afterburner, cx);
                    }
                    0
                }
            }
            2 => {
                if self.break_away.update(cx, dt) != 0 {
                    2
                } else {
                    0
                }
            }
            3 => {
                let back_inside = self
                    .watched_view(cx)
                    .is_some_and(|w| !Self::outside_cone(&w, &own, 0.7));
                if back_inside {
                    2
                } else {
                    self.break_away.break_style = 2;
                    if self.break_away.update(cx, dt) != 0 {
                        2
                    } else {
                        0
                    }
                }
            }
            4 => {
                let best = cx.state.sys.timers.best_time;
                let m = p.missile_reaction;
                let within = |limit: f32| best > 0.0 && best <= limit;
                let mode = if within(m.evade_break_time) {
                    Some(1)
                } else if within(m.evade_slide_time) {
                    Some(2)
                } else if within(m.evade_afterburn_time) {
                    Some(0)
                } else {
                    None
                };
                if let Some(mode) = mode {
                    let missile = cx.state.sys.timers.best_missile;
                    self.dodge_missile.set_watch_object(missile);
                    self.dodge_missile.mode = mode;
                    self.dodge_missile.update(cx, dt);
                }
                1
            }
            _ => 2,
        };
        self.base.result = result;
        result
    }

    /// Slot 10: the op's object (`FindVerifiedObject`), or without one the
    /// closest hostile, becomes the watched object.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let id = op_dword(op, 0x0c);
        self.watched = cx.view_of((id != 0).then_some(id)).map(|v| v.id);
        self.base.flags |= 0x2;
        true
    }

    fn object_left(&mut self, _cx: &mut Cx<'_, W>, id: u32) {
        if self.watched == Some(id) {
            self.watched = None;
        }
    }

    fn target(&self) -> Option<u32> {
        self.watched
    }
}
