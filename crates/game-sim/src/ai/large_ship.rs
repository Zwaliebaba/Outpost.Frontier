//! `LargeShipBehavior` (behaviour 11, "Large Ship (move)",
//! `Common/common_largeshipbehavior.cpp`, class in
//! `include/common_largeshipbehavior.h`): how a cruiser manoeuvres in a
//! fight. Against a station, a capital ship or a solar-like target it
//! moves toward the target; otherwise, every five seconds, it picks a
//! point toward the centre of the hostile ships, kept within 50 m of its
//! own height and short of any ship in the way, and moves and faces there.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx};
use super::face_idea::FaceIdea;
use super::fp::{e, inline_fast_length, r};
use super::intercept::cached_range;
use super::move_idea::MoveIdea;
use super::raycast::ray_sphere_intersect;

/// π (`0x40490fdb`).
const PI: f32 = core::f32::consts::PI;

/// `LargeShipBehavior` (0x17c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct LargeShipBehavior {
    base: BehaviorBase,
    /// +0x2c the guns' average range (1500 until slot 3 measures it).
    pub gun_range: f32,
    /// +0x30 the move idea.
    pub move_idea: MoveIdea,
    /// +0xa8 the face idea.
    pub face: FaceIdea,
    /// +0x168 the clock of the hostile-centre pick.
    pub pick_timer: f32,
    /// +0x16c the hold time of the last goal.
    pub hold_timer: f32,
    /// +0x170 the goal.
    pub goal: Vector,
}

impl LargeShipBehavior {
    /// `LargeShipBehavior::LargeShipBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            gun_range: 1500.0,
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            pick_timer: 0.0,
            hold_timer: 0.0,
            goal: Vector::ZERO,
        }
    }

    /// Both ideas toward `point`, the move idea in mode 0 within `range`.
    fn head_for<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, point: Vector, range: f32) {
        self.move_idea.set_range_3c(range);
        self.move_idea.set_mode(cx, 0);
        self.move_idea.set_scale_40(0.6);
        self.face.set_position(point);
        self.move_idea.set_position(cx, point);
        self.hold_timer = 30.0;
    }

    fn commit_both<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.move_idea.commit(cx);
        self.face.commit(cx);
    }

    /// The body of `Update` once the target is known.
    fn run<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        target_flags: u32,
        threat: Vector,
    ) -> i32 {
        let dt = self.base.tick_delta;
        let mut hold_expired = false;
        self.hold_timer = r(e(self.hold_timer) - e(dt));
        if self.hold_timer <= 0.0 {
            self.hold_timer = 0.0;
            hold_expired = true;
        }
        if target_flags & 0x28_0100 != 0 {
            self.head_for(cx, threat, 500.0);
            return 1;
        }
        self.pick_timer = r(e(self.pick_timer) - e(dt));
        if self.pick_timer < 0.0 {
            self.pick_timer = 5.0;
            if !self.pick(cx, hold_expired) {
                return 1;
            }
        }
        self.face.update(cx);
        self.move_idea.update(cx, dt);
        1
    }

    /// The five-second pick; false when the tick ended inside it (the
    /// goal reached).
    fn pick<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, hold_expired: bool) -> bool {
        let dt = self.base.tick_delta;
        let Some(own) = cx.own() else {
            return true;
        };
        let self_pos = own.position;
        let list = cx.state.sys.targets.views(&mut *cx.world, cx.ship);
        let mut centre = Vector::ZERO;
        let mut count = 0_i32;
        for (h, v) in &list {
            let (attitude, _) = cached_range(&*cx.world, Some(cx.ship), Some(*h));
            if attitude <= -0.6 {
                let p = v.position;
                centre = Vector::new(
                    r(e(centre.x) + e(p.x)),
                    r(e(centre.y) + e(p.y)),
                    r(e(centre.z) + e(p.z)),
                );
                count += 1;
            }
        }
        if count <= 0 || !hold_expired {
            self.commit_both(cx);
            return true;
        }
        let inv = 1.0 / f64::from(count);
        centre = Vector::new(
            r(e(centre.x) * inv),
            r(e(centre.y) * inv),
            r(e(centre.z) * inv),
        );
        let mut nearby = Vector::ZERO;
        let mut nearby_count = 0_i32;
        for (_, v) in &list {
            if v.id == own.id {
                continue;
            }
            let flags = v.type_flags;
            if flags != 0x8_0000 && flags != 0x10_0000 && flags != 0x20_0000 {
                continue;
            }
            let p = v.position;
            let d = Vector::new(
                r(e(p.x) - e(self_pos.x)),
                r(e(p.y) - e(self_pos.y)),
                r(e(p.z) - e(self_pos.z)),
            );
            let dist_sq = (e(d.x) * e(d.x) + e(d.z) * e(d.z)) + e(d.y) * e(d.y);
            if !(inline_fast_length(dist_sq) < 1500.0) {
                continue;
            }
            nearby_count += 1;
            let c = v.center;
            nearby = Vector::new(
                r(e(nearby.x) + e(c.x)),
                r(e(nearby.y) + e(c.y)),
                r(e(nearby.z) + e(c.z)),
            );
        }
        if nearby_count > 1 {
            let inv = 1.0 / f64::from(nearby_count);
            let away = Vector::new(
                r(e(nearby.x) * inv - e(self_pos.x)),
                r(e(nearby.y) * inv - e(self_pos.y)),
                r(e(nearby.z) * inv - e(self_pos.z)),
            );
            let inv_len = 1.0
                / ((e(away.x) * e(away.x) + e(away.z) * e(away.z)) + e(away.y) * e(away.y)).sqrt();
            let push_x = (e(away.x) * inv_len) * 500.0;
            let norm_y = r(e(away.y) * inv_len);
            let norm_z = r(e(away.z) * inv_len);
            let push_y = e(norm_y) * 500.0;
            let push_z = e(norm_z) * 500.0;
            self.goal = Vector::new(
                r(e(centre.x) - push_x),
                r(e(centre.y) - push_y),
                r(e(centre.z) - push_z),
            );
        } else {
            self.goal = centre;
        }
        let ceiling = e(self_pos.y) + 50.0;
        if ceiling < e(self.goal.y) {
            self.goal.y = r(ceiling);
        }
        let floor = e(self_pos.y) - 50.0;
        if floor > e(self.goal.y) {
            self.goal.y = r(floor);
        }
        let to_goal = Vector::new(
            r(e(self.goal.x) - e(self_pos.x)),
            r(e(self.goal.y) - e(self_pos.y)),
            r(e(self.goal.z) - e(self_pos.z)),
        );
        let to_goal_sq = (e(to_goal.x) * e(to_goal.x) + e(to_goal.z) * e(to_goal.z))
            + e(to_goal.y) * e(to_goal.y);
        if inline_fast_length(to_goal_sq) < 100.0 {
            self.face.update(cx);
            self.move_idea.update(cx, dt);
            return false;
        }
        let mut ray = to_goal;
        let mut best = 0.0_f32;
        let mut blocked = false;
        for (_, v) in &list {
            if v.id == own.id {
                continue;
            }
            let flags = v.type_flags;
            if flags & 0xfdff == 0 && flags != 0x20_0000 {
                continue;
            }
            let inv =
                1.0 / ((e(ray.x) * e(ray.x) + e(ray.y) * e(ray.y)) + e(ray.z) * e(ray.z)).sqrt();
            ray = Vector::new(r(e(ray.x) * inv), r(e(ray.y) * inv), r(e(ray.z) * inv));
            let limit = r(e(v.radius) + 500.0);
            let c = v.center;
            let dx = e(self_pos.x) - e(c.x);
            let dy = e(self_pos.y) - e(c.y);
            let dz = e(self_pos.z) - e(c.z);
            if inline_fast_length((dz * dz + dy * dy) + dx * dx) < e(limit) {
                blocked = true;
                continue;
            }
            let Some(hit) = ray_sphere_intersect(self_pos, ray, c, limit) else {
                continue;
            };
            if best == 0.0 || hit < best {
                best = hit;
            }
        }
        if best != 0.0 {
            self.goal.x = r(e(ray.x) * e(best) + e(self_pos.x));
            self.goal.y = r(e(ray.y) * e(best) + e(self_pos.y));
            let scaled_z = r(e(ray.z) * e(best));
            self.goal.z = r(e(scaled_z) + e(self_pos.z));
        }
        if blocked {
            self.commit_both(cx);
        } else {
            let goal = self.goal;
            self.head_for(cx, goal, 250.0);
        }
        true
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for LargeShipBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    /// Slot 1's world half: both ideas get their context, the move idea
    /// enabled.
    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        self.move_idea.set_context(&*cx);
        self.move_idea.set_enabled(1);
        self.face.set_context(&*cx);
    }

    /// Slot 3: the move idea's bearing limit π, no cruise, and the guns'
    /// average range.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.move_idea.set_angle_38(PI);
        cx.set_cruise_lock(0, 3);
        if let Some((average, _)) = cx.state.sys.guns.gun_range_stats(&*cx.world, cx.ship) {
            self.gun_range = average;
        }
    }

    /// Slot 4: both ideas let go.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.face.commit(cx);
        self.move_idea.commit(cx);
        self.base.disengage(cx.state);
    }

    /// Slot 5: a cruiser (type 0x80000) with a target, 1; otherwise 2.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut mode = 2;
        if cx.own().is_some_and(|v| v.type_flags & 0x8_0000 != 0) && cx.target_view().is_some() {
            mode = 1;
        }
        self.base.mode = if self.base.flag29 != 0 { 3 } else { mode };
        self.base.mode
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let Some(target) = cx.target_view() else {
            self.base.result = 2;
            return 2;
        };
        let result = self.run(cx, target.type_flags, target.center);
        self.base.result = result;
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        let l = LargeShipBehavior::new(11);
        assert_eq!(l.gun_range, 1500.0);
        assert_eq!(l.goal, Vector::ZERO);
    }
}
