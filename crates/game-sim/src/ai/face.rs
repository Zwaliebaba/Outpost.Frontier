//! `FaceBehavior` (behaviour 15, `Common/common_facebehavior.cpp`, class
//! in `include/common_facebehavior.h`): turn to face an object (face kind
//! 1), a point (2), an orientation (3) or keep the current heading (4),
//! through the embedded face idea; for AI ships also the engine-kill
//! manoeuvre against the closest hostile (face it with the afterburner
//! idea, then hold it in the sights).
//!
//! `Slot5` asks the formation block's `FollowerDirectiveGateA` and the
//! pathfinder's cached obstacle, and builds the target list for the
//! closest hostile (`FindClosestHostileTarget`).

use neuron_core::math::{Matrix, Vector};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, EvaluateResult};
use super::face_idea::FaceIdea;
use super::fp::{column2, e, neg, r, sub};
use super::idea::{AfterburnerIdea, Idea};
use super::intercept::{get_intercept_clearance, get_intercept_clearance_of_iface};
use super::move_idea::MoveIdea;
use super::nav::with_own_motion;
use super::raycast::bearing_angle_to_object;
use super::steer::{build_orientation_basis, build_perpendicular_frame};

/// π/2 as the float the reference widens (`kWideBearingGate`).
const WIDE_BEARING_GATE: f64 = 1.570_796_370_506_286_6;
/// π/16 (`kFaceLockGate`).
const FACE_LOCK_GATE: f64 = 0.196_349_546_313_285_83;

/// `FaceBehavior` (0x1dc bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FaceBehavior {
    base: BehaviorBase,
    /// +0x2c the face kind (1 to 4).
    pub kind: i32,
    /// +0x30 the watched object (kind 1, or the engine-kill target).
    pub watcher_30: Option<u32>,
    /// +0x38 the frame to take (kind 3, and kind 2's built frame).
    pub orient_38: Matrix,
    /// +0x5c the point (kind 2).
    pub target_5c: Vector,
    /// +0x68 a move idea (bound, never ticked).
    pub move_idea: MoveIdea,
    /// +0xe0 the face idea.
    pub face: FaceIdea,
    /// +0x1a0 the afterburner idea.
    pub afterburner: AfterburnerIdea,
    /// +0x1b4 the player steers.
    pub flag_1b4: u8,
    /// +0x1b8, +0x1bc, +0x1c0 the player's steering triple.
    pub steer: Vector,
    /// +0x1c4 the op's `+0x44`: keep the engine off (1).
    pub flag_1c4: u8,
    /// +0x1c5 the target is in the sights.
    pub flag_1c5: u8,
    /// +0x1c8 the engine-kill clock.
    pub field_1c8: f32,
    /// +0x1cc `get_angular_speed_XY(0, 1)` at bind time.
    pub field_1cc: f32,
    /// +0x1d0 (0.9).
    pub field_1d0: f32,
    /// +0x1d4 finish at the next tick.
    pub flag_1d4: u8,
    /// +0x1d8 the afterburner clock.
    pub field_1d8: f32,
}

impl FaceBehavior {
    /// `FaceBehavior::FaceBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            kind: 0,
            watcher_30: None,
            orient_38: Matrix::default(),
            target_5c: Vector::ZERO,
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            afterburner: AfterburnerIdea::default(),
            flag_1b4: 0,
            steer: Vector::ZERO,
            flag_1c4: 1,
            flag_1c5: 0,
            field_1c8: 0.0,
            field_1cc: 0.0,
            field_1d0: 0.9,
            flag_1d4: 0,
            field_1d8: 0.0,
        }
    }

    /// `FindClosestHostileTarget(&Sys->Timers, MaxClearance)`: the nearest
    /// ship of the target list toward which the owner's attitude is at most
    /// -0.6, within `max_clearance` (a negative bound takes any range) and
    /// 100 km.
    fn closest_hostile<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, max_clearance: f32) -> Option<u32> {
        let own = cx.own()?;
        let mut best = None;
        let mut best_distance = 100_000.0_f32;
        let ids = cx.state.sys.targets.build(&mut *cx.world, cx.ship).to_vec();
        for id in ids {
            let Some(h) = cx.world.handle_of(id) else {
                continue;
            };
            let Some(t) = cx.world.inspect(h) else {
                continue;
            };
            if t.type_flags & 0x7f_0000 == 0 {
                continue;
            }
            let attitude = cx.world.attitude(cx.ship, h).unwrap_or(0.0);
            if !(attitude <= -0.6) {
                continue;
            }
            if !(max_clearance < 0.0) && !(get_intercept_clearance(&own, &t) < e(max_clearance)) {
                continue;
            }
            let (a, b) = (own.center, t.center);
            let dx = e(a.x) - e(b.x);
            let dy = e(a.y) - e(b.y);
            let dz = e(a.z) - e(b.z);
            let distance = ((dx * dx + dz * dz) + dy * dy).sqrt();
            if distance < e(best_distance) {
                best_distance = r(distance);
                best = Some(t.id);
            }
        }
        best
    }

    /// The watched object while it exists.
    fn watched_view<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> Option<super::world::ObjectView> {
        cx.view_of(self.watcher_30)
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for FaceBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.face.set_context(&*cx);
        Idea::<W>::bind(&mut self.afterburner, Some(self.base.owner_id));
        self.field_1cc = with_own_motion(&*cx, |m| m.map_or(0.0, |m| m.angular_speed_xy(0.0, 1.0)));
    }

    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.flag_1c5 = 0;
        self.field_1c8 = 0.0;
        cx.set_cruise_lock(0, 3);
        cx.set_engine_active(self.flag_1c4 == 0);
        self.flag_1b4 = 0;
        self.steer = Vector::ZERO;
        self.field_1d8 = 0.0;
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        if self.base.flags & 0x2 == 0 && self.watcher_30.is_some() {
            self.watcher_30 = None;
        }
        self.face.commit(cx);
        self.afterburner.commit(cx);
        self.base.disengage(cx.state);
    }

    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        if self.base.flags & 0x2 != 0 && self.kind == 1 && self.watched_view(cx).is_some() {
            let mode = if self.base.flag29 != 0 { 3 } else { 1 };
            self.base.mode = mode;
            return mode;
        }
        let mut result = 2;
        let is_player = cx.own().is_some_and(|v| v.is_player);
        let behavior = cx.state.behavior;
        let personality = cx.state.personality;
        let gate = cx
            .state
            .sys
            .formation
            .gate_a(&*cx.world, is_player, behavior, &personality);
        if !gate {
            let p = &cx.state.pathfind;
            if !(p.cached_obstacle_id != 0 && p.has_cached_obstacle) {
                let max = cx.state.personality.engine_kill.max_target_distance;
                self.watcher_30 = Self::closest_hostile(cx, max);
                if let Some(watched) = self.watched_view(cx)
                    && watched.type_flags & 0x3_0000 != 0
                    && let Some(own) = cx.own()
                {
                    let v = own.velocity;
                    let speed_sq = (e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x);
                    if speed_sq > 2500.0 {
                        result = if cx.state.sys.timer3_active() { 0 } else { 1 };
                    }
                }
            }
        }
        if self.base.flag29 != 0 {
            result = 3;
        }
        self.base.mode = result;
        result
    }

    fn evaluate(&mut self, _cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        // `PlayerTrailAvailable` is false in the server build; the
        // retargeting arm is never reached.
        EvaluateResult::Skip
    }

    #[allow(clippy::too_many_lines)]
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if self.flag_1d4 != 0 {
            self.flag_1d4 = 0;
            self.base.result = 2;
            return 2;
        }
        let Some(own) = cx.own() else {
            self.base.result = 3;
            return 3;
        };
        let dt = self.base.tick_delta;
        let kill = cx.state.personality.engine_kill;
        let mut result = 3;
        if !own.is_player {
            let watched = self.watched_view(cx);
            let mut still_turning = false;
            if let Some(w) = &watched
                && kill.use_afterburner != 0.0
                && self.field_1d8 < kill.afterburner_time
            {
                let bearing = bearing_angle_to_object(own.center, &own.orientation, w.center);
                still_turning = bearing > WIDE_BEARING_GATE;
            }
            if still_turning {
                self.field_1d8 = r(e(self.field_1d8) + e(dt));
                Idea::<W>::update(&mut self.afterburner, cx, dt);
                self.face.set_axis(Vector::ZERO);
                self.face.update(cx);
                self.base.result = 0;
                return 0;
            }
            self.afterburner.commit(cx);
            if self.flag_1c5 != 0 {
                if self.field_1c8 < kill.face_time {
                    result = 0;
                    self.field_1c8 = r(e(self.field_1c8) + e(dt));
                } else {
                    self.field_1c8 = 0.0;
                    self.watcher_30 = None;
                    result = 2;
                }
            } else if let Some(w) = self.watched_view(cx) {
                self.kind = 1;
                let angle = bearing_angle_to_object(w.center, &w.orientation, own.center);
                if angle > FACE_LOCK_GATE {
                    if !(self.field_1c8 < kill.search_time) {
                        result = 3;
                    } else {
                        result = 0;
                        self.field_1c8 = r(e(self.field_1c8) + e(dt));
                    }
                } else {
                    self.flag_1c5 = 1;
                    self.field_1c8 = 0.0;
                    result = 1;
                    let clearance = get_intercept_clearance_of_iface(&own, &w);
                    if clearance > e(kill.max_target_distance) {
                        result = 2;
                    }
                }
            }
        }

        match self.kind {
            1 => {
                if self.flag_1b4 != 0 {
                    self.face.set_axis(self.steer);
                } else {
                    let watched = self.watched_view(cx).map(|v| v.id);
                    self.face.set_watch_object(watched);
                    if watched.is_none() {
                        self.base.result = 3;
                        return 3;
                    }
                }
            }
            2 => {
                if self.flag_1b4 != 0 {
                    let forward = neg(column2(&own.orientation));
                    let axis = self.steer;
                    let mag =
                        (e(axis.x) * e(axis.x) + e(axis.y) * e(axis.y)) + e(axis.z) * e(axis.z);
                    if mag > 0.0 {
                        self.face.set_axis(axis);
                    } else {
                        self.face.set_direction(forward);
                    }
                } else {
                    let diff = sub(self.target_5c, own.center);
                    let mut built = false;
                    if !cx.check_steering_gate()
                        && let Some(frame) = build_perpendicular_frame(diff, 0.999, None)
                    {
                        self.orient_38 = frame;
                        built = true;
                    }
                    if !built {
                        self.orient_38 = build_orientation_basis(&own.orientation, diff, None);
                    }
                    self.face.set_basis(&self.orient_38);
                }
            }
            3 => self.face.set_basis(&self.orient_38),
            4 => {
                let forward = neg(column2(&own.orientation));
                self.face.set_direction(forward);
            }
            _ => {}
        }
        cx.set_engine_active(self.flag_1c4 == 0);
        self.face.update(cx);
        if own.is_player {
            result = 1;
        }
        self.base.result = result;
        result
    }

    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let DirectiveOp::Face(op) = op else {
            self.base.flags &= !0x2;
            return false;
        };
        self.flag_1c4 = op.field_44;
        self.kind = op.mode;
        match op.mode {
            1 => {
                self.watcher_30 = cx.view_of(Some(op.target)).map(|v| v.id);
                if self.watcher_30.is_none() {
                    self.base.flags &= !0x2;
                    return false;
                }
                self.base.flags |= 0x2;
                true
            }
            2 => {
                self.target_5c = op.point;
                self.base.flags |= 0x2;
                true
            }
            3 => {
                self.orient_38 = op.orientation;
                self.base.flags |= 0x2;
                true
            }
            4 => {
                self.base.flags |= 0x2;
                self.kind = 4;
                true
            }
            _ => {
                self.base.flags &= !0x2;
                false
            }
        }
    }

    /// `FaceBehavior::set_throttle` (slot 13): the throttle is unread; the
    /// next update ends the behaviour (`Flag1d4`).
    fn set_throttle(&mut self, _cx: &mut Cx<'_, W>, _throttle: f32) {
        self.flag_1d4 = 1;
    }

    fn set_direction(&mut self, cx: &mut Cx<'_, W>, offset: Vector) {
        if self.kind != 2 {
            let mag =
                r((e(offset.x) * e(offset.x) + e(offset.y) * e(offset.y))
                    + e(offset.z) * e(offset.z));
            if self.kind != 1 || !(mag > 0.0) {
                self.flag_1b4 = 0;
                return;
            }
        }
        self.flag_1b4 = 1;
        let mut x = -offset.y;
        let mut y = -offset.x;
        let sensitivity = cx.state.turn_sensitivity;
        if sensitivity > 0.0 {
            let rescale = 1.0 / e(sensitivity);
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
        self.steer = Vector::new(clamp(x), clamp(y), offset.z);
    }

    fn target(&self) -> Option<u32> {
        self.watcher_30
    }
}
