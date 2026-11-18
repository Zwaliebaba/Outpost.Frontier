//! `MatchThrottle2Idea` (`Common/common_matchthrottleidea.cpp`, class in
//! `include/common_idea.h`): the idea that matches a watched ship's speed
//! (and cruise state, and optionally its trade-lane nudge), holding a
//! standoff distance or a point relative to it, and catching up when far.
//!
//! The state is the C++ object's own fields; the `BaseIdea` plumbing is
//! the [`NavCx`] each call takes, as for [`super::move_idea::MoveIdea`].
//! The system manager's `Timers.IsTimer4Active()` is a `bool` argument of
//! [`MatchThrottle2Idea::update`]; the [`Idea`] implementation passes
//! false (the timer table is not ported).

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Cx};
use super::fp::{add, column2, e, inline_fast_length, len_sq_xyz, neg, normalize3, r, sub};
use super::idea::{Idea, IdeaBase};
use super::intercept::{iface_position_or_zero, signed_radial_clearance_to_point};
use super::nav::{
    NavCx, max_scalar_attribute, projected_throttle, query_falloff_effect, query_radius_effect,
    speed, watched, with_motion_of, with_own_motion,
};
use super::raycast::{bearing_angle_to_point, clamped_dot3};
use super::world::{AiCommand, AiWorld, ThrustEquipType};

/// `MatchThrottle2Idea::GetKind`.
pub const MATCH_THROTTLE_IDEA_KIND: i16 = 6;

/// `MatchThrottle2Idea` (`common_idea.h`), its fields from `+0x14`.
#[derive(Clone, Debug, PartialEq)]
pub struct MatchThrottle2Idea {
    /// `BaseIdea`'s fields.
    pub base: IdeaBase,
    /// +0x14 the ship being matched (its id).
    pub watcher: Option<u32>,
    /// +0x1c the point (`SetWatchAndPosition`); `update` stores the
    /// watched centre there in standoff mode.
    pub position: Vector,
    /// +0x28 the standoff distance (`SetWatchAndDistance`); the
    /// constructor leaves it unwritten, 0 here.
    pub distance_28: f32,
    /// +0x2c the closing-speed normaliser.
    pub speed_2c: f32,
    /// +0x30 the throttle correction.
    pub throttle_bias_30: f32,
    /// +0x34 the bearing beyond which the idea only coasts (π/4).
    pub max_bearing_34: f32,
    /// +0x38 follow the watched ship's cruise state (1).
    pub flag_38: u8,
    /// +0x39 `Commit` keeps the engine and cruise state when set.
    pub flag_39: u8,
    /// +0x3a match the watched ship's nudge.
    pub flag_3a: u8,
    /// +0x3b thrusters allowed.
    pub flag_3b: u8,
    /// +0x3c catch up beyond the catch-up range (1).
    pub flag_3c: u8,
    /// +0x3d catching up this tick.
    pub flag_3d: u8,
    /// +0x40 below this closing distance the bias is the closing bias
    /// (-200).
    pub closing_floor_40: f32,
    /// +0x44 (-0.5).
    pub closing_bias_44: f32,
    /// +0x48 (0.05), never read.
    pub field_48: f32,
    /// +0x4c the cruise speed's time to accelerate plus 300.
    pub speed_4c: f32,
    /// +0x50 the engine speed's time to accelerate plus 50.
    pub speed_50: f32,
    /// +0x54 closing distances within this leave the bias at 0 (5).
    pub closing_window_54: f32,
    /// +0x58 the catch-up range (500).
    pub catch_up_range_58: f32,
}

impl Default for MatchThrottle2Idea {
    fn default() -> Self {
        Self::new()
    }
}

impl MatchThrottle2Idea {
    /// `MatchThrottle2Idea::MatchThrottle2Idea` (`common_matchthrottleidea.cpp:52`).
    #[must_use]
    pub fn new() -> Self {
        Self {
            base: IdeaBase::default(),
            watcher: None,
            position: Vector::ZERO,
            distance_28: 0.0,
            speed_2c: 0.0,
            throttle_bias_30: 0.0,
            max_bearing_34: f32::from_bits(0x3f49_0fdb),
            flag_38: 1,
            flag_39: 0,
            flag_3a: 0,
            flag_3b: 0,
            flag_3c: 1,
            flag_3d: 0,
            closing_floor_40: -200.0,
            closing_bias_44: -0.5,
            field_48: f32::from_bits(0x3d4c_cccd),
            speed_4c: 0.0,
            speed_50: 0.0,
            closing_window_54: 5.0,
            catch_up_range_58: 500.0,
        }
    }

    /// `MatchThrottle2Idea::SetContext` (`common_matchthrottleidea.cpp:78`):
    /// the cruise and engine normalisers from the ship's motion model;
    /// false when the ship is gone.
    pub fn set_context<C: NavCx + ?Sized>(&mut self, cx: &C) -> bool {
        let Some(view) = cx.world().inspect(cx.ship()) else {
            return false;
        };
        self.base.owner = Some(view.id);
        self.base.attached = true;
        let (speed_4c, speed_50) = with_own_motion(cx, |motion| {
            let cruise = speed(motion, 1.0, ThrustEquipType::Cruise);
            let speed_4c = r(e(query_radius_effect(
                motion,
                0.0,
                cruise,
                0.0,
                ThrustEquipType::Cruise,
            )) + e(300.0));
            let engine = speed(motion, 1.0, ThrustEquipType::Engine);
            let speed_50 = r(e(query_radius_effect(
                motion,
                0.0,
                engine,
                0.0,
                ThrustEquipType::Engine,
            )) + e(50.0));
            (speed_4c, speed_50)
        });
        self.speed_4c = speed_4c;
        self.speed_50 = speed_50;
        true
    }

    /// `MatchThrottle2Idea::Slot2` (`common_matchthrottleidea.cpp:108`):
    /// follows the watched ship into cruise (above 30 m/s) and keeps cruise
    /// engaged while not catching up.
    pub fn slot2<C: NavCx + ?Sized>(&self, cx: &mut C) {
        let Some(watched) = watched(&*cx, self.watcher) else {
            return;
        };
        if cx.world().inspect(cx.ship()).is_none() {
            return;
        }
        let thrust = cx;
        if self.flag_38 == 0 {
            thrust.engage_cruise(false);
            return;
        }
        let cruise = watched.cruise.is_some_and(|c| c.0);
        let v = watched.velocity;
        let engaged = thrust.cruise_engaged();
        if cruise {
            let speed_sq = (e(v.y) * e(v.y) + e(v.z) * e(v.z)) + e(v.x) * e(v.x);
            if speed_sq > e(900.0) && !engaged {
                thrust.engage_cruise(true);
            }
            return;
        }
        if engaged && self.flag_3d == 0 {
            thrust.engage_cruise(true);
        }
    }

    /// `MatchThrottle2Idea::Commit` (`common_matchthrottleidea.cpp:144`).
    pub fn commit<C: NavCx + ?Sized>(&self, cx: &mut C) {
        cx.set_nudge_direction(Vector::ZERO);
        if self.flag_39 == 0 {
            cx.set_throttle(0.0, 0);
            cx.engage_cruise(false);
        }
        cx.set_thrusters_active(false);
        cx.pathfind_mut().cached_target_position = Vector::ZERO;
    }

    /// `MatchThrottle2Idea::SetWatchAndPosition` (`common_matchthrottleidea.cpp:159`):
    /// the watched ship's position mirrored into the pathfinder (the
    /// origin when it cannot be inspected; D162).
    pub fn set_watch_and_position<C: NavCx + ?Sized>(
        &mut self,
        cx: &mut C,
        object: Option<u32>,
        point: Vector,
    ) {
        self.watcher = object;
        self.position = point;
        self.distance_28 = 0.0;
        self.mirror(cx);
    }

    /// `MatchThrottle2Idea::SetWatchAndDistance` (`common_matchthrottleidea.cpp:172`).
    pub fn set_watch_and_distance<C: NavCx + ?Sized>(
        &mut self,
        cx: &mut C,
        object: Option<u32>,
        distance: f32,
    ) {
        self.watcher = object;
        self.position = Vector::ZERO;
        self.distance_28 = distance;
        self.mirror(cx);
    }

    fn mirror<C: NavCx + ?Sized>(&self, cx: &mut C) {
        let position = watched(&*cx, self.watcher).map_or(Vector::ZERO, |v| v.position);
        cx.pathfind_mut().cached_target_position = position;
    }

    /// `MatchThrottle2Idea::Update` (`common_matchthrottleidea.cpp:184`):
    /// one tick of matching. 3 without a watched ship or an object, else 0.
    /// With the nudge flag set and the pathfinder's bit 0 clear, the
    /// watched ship's nudge is copied as an `IObjAffect::set_nudge_vec`
    /// command on `ship`.
    pub fn update<C: NavCx + ?Sized>(&mut self, cx: &mut C, timer4_active: bool) -> i32 {
        self.flag_3d = 0;
        let Some(watched_id) = self.watcher else {
            return 3;
        };
        let (Some(watched), Some(own)) = (
            watched(&*cx, Some(watched_id)),
            cx.world().inspect(cx.ship()),
        ) else {
            return 3;
        };
        let center = own.center;
        let delta = sub(center, watched.position);
        let distance = r(inline_fast_length(len_sq_xyz(delta)));

        let (cruise, lane) = if self.flag_38 != 0 {
            watched.cruise.unwrap_or((false, false))
        } else {
            (false, false)
        };
        let watched_speed = r(len_sq_xyz(watched.velocity).sqrt());
        let own_speed = r(len_sq_xyz(own.velocity).sqrt());
        let match_type = if cruise && !lane {
            ThrustEquipType::Cruise
        } else {
            ThrustEquipType::Engine
        };
        let (match_throttle, nudge_throttle_of) = {
            let nudge_len = r(len_sq_zyx(watched.nudge.unwrap_or(Vector::ZERO)).sqrt());
            let nudge_speed = with_motion_of(&*cx, Some(watched_id), |watched_motion| {
                speed(watched_motion, nudge_len, ThrustEquipType::Tradelane)
            });
            with_own_motion(&*cx, |motion| {
                (
                    projected_throttle(motion, watched_speed, match_type),
                    projected_throttle(motion, nudge_speed, ThrustEquipType::Tradelane),
                )
            })
        };

        let aim = if self.distance_28 > 0.0 {
            let wc = iface_position_or_zero(Some(&watched));
            let aim = if e(self.distance_28) * 0.95 > e(distance) {
                Vector::ZERO
            } else {
                let standoff =
                    r((e(max_scalar_attribute(&watched)) + e(own.radius)) + e(self.distance_28));
                let away_dir = normalize3(sub(center, wc));
                let scaled = Vector::new(
                    r(e(standoff) * e(away_dir.x)),
                    r(e(standoff) * e(away_dir.y)),
                    r(e(standoff) * e(away_dir.z)),
                );
                sub(add(scaled, wc), center)
            };
            self.position = wc;
            aim
        } else {
            sub(self.position, center)
        };

        let watched_fwd = neg(column2(&watched.orientation));
        let own_fwd = neg(column2(&own.orientation));
        let facing = (e(own_fwd.x) * e(watched_fwd.x) + e(own_fwd.z) * e(watched_fwd.z))
            + e(own_fwd.y) * e(watched_fwd.y);

        let mut throttle;
        if !(facing > 0.0) && !(self.distance_28 > 0.0) {
            throttle = 1.0;
        } else {
            let aim_dir = normalize3(aim);
            let cos = clamped_dot3(watched_fwd, aim_dir);
            let aim_len =
                ((e(aim.x) * e(aim.x) + e(aim.z) * e(aim.z)) + e(aim.y) * e(aim.y)).sqrt();
            let closing = r(aim_len * cos);
            let mode = if cruise {
                self.speed_2c = self.speed_4c;
                4
            } else {
                self.speed_2c = self.speed_50;
                1
            };
            self.throttle_bias_30 = 0.0;
            if self.closing_window_54 < closing.abs() {
                self.throttle_bias_30 = r(e(closing) / e(self.speed_2c));
                if e(watched_speed) > 0.01 && own_speed > watched_speed && closing > 0.0 {
                    let kind = ThrustEquipType::from_value(mode);
                    let slow_distance = with_own_motion(&*cx, |motion| {
                        let slow_time =
                            query_radius_effect(motion, 0.0, own_speed, watched_speed, kind);
                        query_falloff_effect(motion, 0.0, own_speed, slow_time, kind)
                    });
                    self.speed_2c = slow_distance;
                    if closing <= slow_distance && e(slow_distance) > 0.01 {
                        self.throttle_bias_30 = 0.0;
                    }
                }
                if closing < self.closing_floor_40 {
                    self.throttle_bias_30 = self.closing_bias_44;
                }
            }
            throttle = r(e(match_throttle) + e(self.throttle_bias_30));
            if throttle < 0.0 {
                throttle = 0.0;
            }
        }

        let mut mode = if cruise {
            if match_throttle > 0.8 && throttle < 0.8 {
                throttle = 0.8;
            }
            2
        } else {
            0
        };

        let bearing = bearing_angle_to_point(own.center, &own.orientation, self.position);
        if bearing > e(self.max_bearing_34)
            && signed_radial_clearance_to_point(&own, self.position) > e(200.0)
        {
            cx.set_throttle(if mode == 2 { throttle } else { 0.0 }, mode);
        } else {
            if self.flag_3c != 0 && distance > self.catch_up_range_58 {
                self.flag_3d = 1;
                throttle = 1.2;
                mode = 2;
            }
            if self.flag_3b != 0 || timer4_active {
                if !own.is_player && mode == 0 {
                    cx.set_thrusters_active(throttle > 2.0);
                }
            } else {
                cx.set_thrusters_active(false);
            }
            cx.set_throttle(throttle, mode);
        }

        if cx.pathfind().field_214 & 1 != 0 || self.flag_3a == 0 {
            return 0;
        }
        let nudge = watched.nudge.unwrap_or(Vector::ZERO);
        let nudge_len = len_sq_zyx(nudge).sqrt();
        if !(nudge_len > e(0.1)) {
            cx.set_nudge_direction(Vector::ZERO);
            return 0;
        }
        let dir = normalize3(nudge);
        let out = Vector::new(
            r(e(nudge_throttle_of) * e(dir.x)),
            r(e(nudge_throttle_of) * e(dir.y)),
            r(e(nudge_throttle_of) * e(dir.z)),
        );
        cx.command(AiCommand::SetNudge(out));
        0
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for MatchThrottle2Idea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, _dt: f32) -> i32 {
        MatchThrottle2Idea::update(self, cx, false)
    }

    /// (2) `Slot2`, the cruise follow.
    fn tick(&mut self, cx: &mut Cx<'_, W>, _dt: f32) {
        self.slot2(cx);
    }

    fn kind(&self) -> i16 {
        MATCH_THROTTLE_IDEA_KIND
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        MatchThrottle2Idea::commit(self, cx);
    }
}

/// `(z*z + y*y) + x*x`, unrounded.
fn len_sq_zyx(v: Vector) -> f64 {
    (e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::move_idea::tests::{TestCx, Two};

    #[test]
    fn matches_a_ship_ahead() {
        // The watched ship 1000 ahead, flying away along -Z at 50.
        let mut w = Two::new(Vector::ZERO, Vector::new(0.0, 0.0, -1000.0));
        w.other.velocity = Vector::new(0.0, 0.0, -50.0);
        let mut cx = TestCx::new(w);
        let mut idea = MatchThrottle2Idea::new();
        assert!(idea.set_context(&cx));
        // Motion: time_to_accelerate 0, speed = throttle * 100.
        assert_eq!((idea.speed_4c, idea.speed_50), (300.0, 50.0));
        idea.set_watch_and_distance(&mut cx, Some(2), 100.0);
        assert_eq!(
            cx.pathfind.cached_target_position,
            Vector::new(0.0, 0.0, -1000.0)
        );
        assert_eq!(idea.update(&mut cx, false), 0);
        // Far beyond the catch-up range: full cruise throttle.
        assert_eq!(idea.flag_3d, 1);
        assert_eq!(cx.thrust.calls, ["thrusters false", "throttle 1.2 2"]);
        assert_eq!(idea.position, Vector::new(0.0, 0.0, -1000.0));
        idea.slot2(&mut cx);
        idea.commit(&mut cx);
        assert_eq!(cx.pathfind.cached_target_position, Vector::ZERO);
        idea.set_watch_and_position(&mut cx, None, Vector::ZERO);
        assert_eq!(idea.update(&mut cx, false), 3);
    }

    #[test]
    fn close_behind_matches_speed() {
        let mut w = Two::new(Vector::new(0.0, 0.0, -50.0), Vector::new(0.0, 0.0, -200.0));
        w.other.velocity = Vector::new(0.0, 0.0, -50.0);
        let mut cx = TestCx::new(w);
        let mut idea = MatchThrottle2Idea::new();
        idea.set_context(&cx);
        idea.set_watch_and_distance(&mut cx, Some(2), 100.0);
        idea.update(&mut cx, false);
        assert_eq!(idea.flag_3d, 0);
        assert_eq!(cx.thrust.calls.len(), 2, "{:?}", cx.thrust.calls);
        assert!(cx.thrust.calls[1].ends_with(" 0"), "{:?}", cx.thrust.calls);
    }
}
