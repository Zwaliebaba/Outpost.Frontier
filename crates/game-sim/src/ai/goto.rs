//! `GotoBehavior` (behaviour 2, `Common/common_gotobehavior.cpp`, class in
//! `include/common_gotobehavior.h`) and `FreeFlightBehavior` (behaviour
//! 19, `common_freeflightbehavior.cpp`), which derives from it: fly to a
//! watched object (goto kind 0), a point (1) or along a spline (2), with
//! the embedded move, face and spline-move ideas, the proximity watcher at
//! `+0x370` and the formation envelope of the system manager.
//!
//! The proximity watcher's tether events go to the content callback
//! (`IBehaviorManager::NotifyTetherEvent`), queued for after the tick.

use neuron_core::math::{Vector, transform_matrix};
use neuron_core::util::spline::CrSplineSegment;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, EvaluateResult, GotoOp};
use super::face_idea::FaceIdea;
use super::fp::{column2, e, neg, normalize3, r, sub};
use super::idea::Idea;
use super::intercept::{
    get_intercept_clearance_of_iface, iface_position_or_zero, negated_iface_direction,
    signed_radial_clearance_to_point,
};
use super::move_idea::MoveIdea;
use super::nav::{
    NavCx, projected_axis_throttle_xy, projected_throttle, speed, with_motion_of, with_own_motion,
};
use super::raycast::clamped_dot3;
use super::spline_move::SplineMoveIdea;
use super::world::{AiCommand, AiWorld, StrafeDir, ThrustEquipType};

/// `pi` as the float the reference pushes (`0x40490fdb`).
const PI_F: f32 = core::f32::consts::PI;

/// `FormationFollowerEnvelope`, what `ComputeFormationFollowerEnvelope`
/// writes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FormationEnvelope {
    /// +0x00.
    pub speed: f32,
    /// +0x04.
    pub throttle: f32,
    /// +0x08.
    pub angular_speed: f32,
    /// +0x0c.
    pub angular_throttle: f32,
    /// +0x10 the formation is tight (or the ship cruises).
    pub flag_10: bool,
}

/// `FGunConvergenceStateBlock::ComputeFormationFollowerEnvelope`: how fast
/// a formation leader may fly and turn so its followers keep up. Whether
/// it is valid (a ship with followers), and the envelope, whose first
/// four fields the reference writes before the follower count is tested
/// (the rest of the zero-initialised block otherwise). The narrow and wide
/// angles are the block's constructor values (0.1 and 0.2 rad).
pub fn formation_follower_envelope<C: NavCx + ?Sized>(cx: &C) -> (bool, FormationEnvelope) {
    let mut out = FormationEnvelope::default();
    let Some(own) = cx.world().inspect(cx.ship()) else {
        return (false, out);
    };
    let cruise = own.cruise.is_some_and(|c| c.0);
    let mut min_angular = f32::MAX;
    let kind = if cruise {
        ThrustEquipType::Cruise
    } else {
        ThrustEquipType::Engine
    };
    let mut min_speed = with_own_motion(cx, |m| speed(m, 1.0, kind));
    out.speed = min_speed;
    out.throttle = 1.0;
    out.angular_throttle = 1.0;
    out.flag_10 = true;
    if !own.is(0x503) {
        return (false, out);
    }
    let followers = own.formation_followers.clone();
    if followers.is_empty() {
        return (false, out);
    }
    let owner_facing = negated_iface_direction(Some(&own));
    let mut max_projection = -10000.0_f32;
    let mut min_align_cos = 1.0_f32;
    for id in followers {
        let Some(h) = cx.world().handle_of(id) else {
            continue;
        };
        let Some(follower) = cx.world().inspect(h) else {
            continue;
        };
        let (follower_speed, follower_angular) = with_motion_of(cx, Some(id), |m| {
            (
                m.map_or(-1.0, |m| m.speed(1.0, kind)),
                m.map_or(-1.0, |m| m.angular_speed_xy(1.0, 0.0)),
            )
        });
        if follower_speed < min_speed {
            min_speed = follower_speed;
        }
        if follower_angular < min_angular {
            min_angular = follower_angular;
        }
        let offset = follower.formation_offset.unwrap_or(Vector::ZERO);
        let rotated = transform_matrix(&own.orientation, offset);
        let desired = Vector::new(
            r(e(rotated.x) + e(own.center.x)),
            r(e(rotated.y) + e(own.center.y)),
            r(e(rotated.z) + e(own.center.z)),
        );
        let delta = sub(desired, iface_position_or_zero(Some(&follower)));
        let back = neg(column2(&own.orientation));
        let cosine = clamped_dot3(normalize3(delta), back);
        let distance =
            ((e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y)) + e(delta.x) * e(delta.x)).sqrt();
        let projection = cosine * distance;
        if projection > e(max_projection) {
            max_projection = r(projection);
        }
        let follower_facing = negated_iface_direction(Some(&follower));
        let align = clamped_dot3(owner_facing, follower_facing);
        if align < e(min_align_cos) {
            min_align_cos = r(align);
        }
    }
    let divisor: f32 = if cruise { 1000.0 } else { 300.0 };
    if max_projection > -20.0 {
        let mut blend = e(max_projection) / e(divisor);
        if max_projection < 20.0 {
            blend *= blend;
            if blend < e(0.01) {
                blend = 0.0;
            }
        }
        let mut target = r(e(min_speed) - blend * e(min_speed));
        let floor = e(min_speed) * 0.5;
        if e(target) < floor {
            target = r(floor);
        }
        out.speed = target;
        out.throttle = with_own_motion(cx, |m| projected_throttle(m, target, kind));
        let angle_exact = e(min_align_cos).acos();
        let angle = r(angle_exact);
        let (narrow, wide) = (0.1_f32, 0.2_f32);
        let mut angular = if angle_exact < e(narrow) {
            f32::MAX
        } else if angle < wide {
            let mut a = r((e(wide) - e(angle)) * (e(min_angular) / e(wide)));
            let floor = e(min_angular) * 0.5;
            if e(a) < floor {
                a = r(floor);
            }
            a
        } else {
            r(e(min_angular) * 0.2)
        };
        if angular > min_angular {
            angular = min_angular;
        }
        out.angular_speed = angular;
        out.angular_throttle = with_own_motion(cx, |m| projected_axis_throttle_xy(m, angular));
    } else {
        let (s, a) = with_own_motion(cx, |m| {
            (
                speed(m, 1.0, kind),
                m.map_or(0.0, |m| m.angular_speed_xy(1.0, 1.0)),
            )
        });
        out.speed = s;
        out.throttle = 1.0;
        out.angular_speed = a;
        out.angular_throttle = 1.0;
    }
    if !cruise {
        out.flag_10 = max_projection < 20.0 && e(min_align_cos) > 0.99;
    }
    (true, out)
}

/// The shared 0x1c-byte proximity watcher (`GotoBehavior+0x370`): an
/// object, two clearance thresholds scaled by a cruise ratio while the
/// object cruises, and the zone bits of this tick and the last.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProxWatcher {
    /// +0x00 the watched object's id.
    pub watched: Option<u32>,
    /// +0x08 the near threshold.
    pub near: f32,
    /// +0x0c the far threshold.
    pub far: f32,
    /// +0x10 the cruise ratio (at least 1).
    pub ratio: f32,
    /// +0x14 the zone bits: 1 inside the radial clearance, 2 near, 4 far.
    pub zone_flags: u32,
    /// +0x18 last tick's zone bits.
    pub saved_flags: u32,
}

impl ProxWatcher {
    /// `ProxWatcherInit`.
    pub(crate) fn init<C: NavCx + ?Sized>(
        &mut self,
        cx: &C,
        target: Option<u32>,
        near: f32,
        far: f32,
        cruise_ratio: f32,
    ) {
        self.near = near;
        self.far = far;
        self.watched = target.filter(|&id| cx.world().handle_of(id).is_some());
        self.zone_flags = 0;
        self.saved_flags = 0;
        if self.watched.is_none() {
            return;
        }
        if cruise_ratio > 0.0 {
            self.ratio = cruise_ratio;
        } else {
            let own = with_own_motion(cx, |m| speed(m, 1.0, ThrustEquipType::Cruise));
            let other =
                with_motion_of(cx, self.watched, |m| speed(m, 1.0, ThrustEquipType::Engine));
            self.ratio = r(e(own) / e(other));
        }
        if self.ratio < 1.0 {
            self.ratio = 1.0;
        }
    }

    /// `ProxWatcherRecomputeZoneFlags`.
    pub(crate) fn recompute_zone_flags<C: NavCx + ?Sized>(&mut self, cx: &C, ref_point: Vector) {
        let Some(obj) = super::nav::watched(cx, self.watched) else {
            return;
        };
        let Some(own) = cx.world().inspect(cx.ship()) else {
            return;
        };
        self.saved_flags = self.zone_flags;
        let pos = obj.position;
        let dx = r(e(pos.x) - e(ref_point.x));
        let dy = r(e(pos.y) - e(ref_point.y));
        let dz = r(e(pos.z) - e(ref_point.z));
        let radial = signed_radial_clearance_to_point(&own, ref_point);
        self.zone_flags = 0;
        let distance = ((e(dz) * e(dz) + e(dx) * e(dx)) + e(dy) * e(dy)).sqrt();
        if radial > distance {
            self.zone_flags = 1;
        }
        let clearance = e(r(get_intercept_clearance_of_iface(&own, &obj)));
        let cruise = obj.cruise.is_some_and(|c| c.0);
        let (near, far) = if cruise {
            (e(self.near) * e(self.ratio), e(self.far) * e(self.ratio))
        } else {
            (e(self.near), e(self.far))
        };
        let band = 50.0;
        if clearance <= near - band {
            self.zone_flags |= 2;
        } else if clearance <= near + band {
            self.zone_flags |= self.saved_flags;
        }
        if clearance <= far - band {
            self.zone_flags |= 4;
            return;
        }
        if clearance <= far + band {
            self.zone_flags |= self.saved_flags;
        }
    }

    /// `ProxWatcherComputeSpeedCap(Owner, Sys, Selector, Value)`.
    pub(crate) fn speed_cap<C: NavCx + ?Sized>(&self, cx: &C, selector: i32, value: f32) -> f64 {
        let Some(obj) = super::nav::watched(cx, self.watched) else {
            return 0.0;
        };
        let kind = ThrustEquipType::from_value(selector);
        let query = if selector == 1 { value } else { 1.0 };
        let owner_speed = with_own_motion(cx, |m| speed(m, query, kind));
        let zone_speed = if self.zone_flags & 3 != 0 {
            e(with_motion_of(cx, self.watched, |m| speed(m, 1.0, kind)))
        } else if self.zone_flags & 4 == 0 {
            0.0
        } else {
            let Some(own) = cx.world().inspect(cx.ship()) else {
                return 0.0;
            };
            let clearance = get_intercept_clearance_of_iface(&own, &obj);
            let blend = r(1.0 - ((clearance - e(self.near)) / (e(self.far) - e(self.near))));
            let full = with_motion_of(cx, self.watched, |m| speed(m, 1.0, kind));
            let mut zone = (e(full) * e(blend)) * e(blend);
            if zone < 10.0 {
                zone = 0.0;
            }
            zone
        };
        if !(zone_speed > e(owner_speed)) {
            zone_speed
        } else {
            e(owner_speed)
        }
    }

    /// `ProxWatcherNotifyZoneChange(Watcher, NavHelper, BehaviorMgr,
    /// RefPoint)`: a zone change of a watched object that is still there
    /// goes to `IBehaviorManager::NotifyTetherEvent` with the reference
    /// point, which reports it while the ship is in an attack state and its
    /// current behaviour is the goto (2) or the dock (7).
    pub(crate) fn notify_zone_change<W: super::behavior::AiEnv + ?Sized>(
        &self,
        cx: &mut Cx<'_, W>,
        ref_point: Vector,
    ) {
        let Some(code) = self.zone_change() else {
            return;
        };
        let Some(watched) = cx.view_of(self.watched).map(|v| v.id) else {
            return;
        };
        let own = cx.own().map_or(0, |v| v.id);
        let event = super::content::CombatEvent::Tether {
            own,
            watched,
            status: code,
            position: ref_point,
        };
        let _ = cx.notify_content(&[2, 7], event);
    }

    /// `ProxWatcherNotifyZoneChange`: the tether event code (1 entered the
    /// near zone, 2 entered the far zone, 3 left it), or none.
    pub(crate) fn zone_change(&self) -> Option<i32> {
        self.watched?;
        let (saved, now) = (self.saved_flags, self.zone_flags);
        if saved & 4 != 0 && saved & 2 == 0 && now & 2 != 0 {
            Some(1)
        } else if saved & 4 == 0 && now & 4 != 0 {
            Some(2)
        } else if saved & 4 != 0 && now & 4 == 0 {
            Some(3)
        } else {
            None
        }
    }
}

/// `GotoBehavior` (0x3fc bytes) and, with `free_flight`, its subclass
/// `FreeFlightBehavior` (0x400).
#[derive(Clone, Debug, PartialEq)]
pub struct GotoBehavior {
    base: BehaviorBase,
    /// The `FreeFlightBehavior` overrides (slots 3, 4, 6, 10, 15).
    pub free_flight: bool,
    /// +0x2c the move idea.
    pub move_idea: MoveIdea,
    /// +0xa4 the face idea.
    pub face: FaceIdea,
    /// +0x164 the spline-move idea.
    pub spline_idea: SplineMoveIdea,
    /// +0x2d8, +0x2dc, +0x2e0 the player's steering triple.
    pub steer: Vector,
    /// +0x2e4 the player's throttle.
    pub field_2e4: f32,
    /// +0x2e8 the player's cruise.
    pub flag_2e8: u8,
    /// +0x2e9 the player's engine (1).
    pub flag_2e9: u8,
    /// +0x2ea the player steers.
    pub flag_2ea: u8,
    /// +0x2eb the player set a throttle.
    pub flag_2eb: u8,
    /// +0x2ec the player brakes.
    pub flag_2ec: u8,
    /// +0x2f0 (1.5).
    pub field_2f0: f32,
    /// +0x2f4 the goto kind: 0 an object, 1 a point, 2 a spline (3).
    pub kind: i32,
    /// +0x2f8 the lead point.
    pub lead: Vector,
    /// +0x304 the destination.
    pub destination: Vector,
    /// +0x310 the watched destination object.
    pub watcher_310: Option<u32>,
    /// +0x318 the spline.
    pub spline: Option<CrSplineSegment>,
    /// +0x358 the arrival range (200).
    pub range: f32,
    /// +0x35c the speed cap (-1).
    pub field_35c: f32,
    /// +0x360 the throttle for that speed (1).
    pub throttle_360: f32,
    /// +0x364 the destination is far enough to cruise.
    pub flag_364: u8,
    /// +0x365 decide `flag_364` at the next tick (1).
    pub flag_365: u8,
    /// +0x366 the op's `+0x58` (stop at the destination).
    pub flag_366: u8,
    /// +0x367 the op's `+0x59`.
    pub flag_367: u8,
    /// +0x368 the move idea cruises.
    pub flag_368: u8,
    /// +0x369.
    pub flag_369: u8,
    /// +0x36a force cruise.
    pub flag_36a: u8,
    /// +0x36b forbid cruise.
    pub flag_36b: u8,
    /// +0x36c wait for the face idea (1).
    pub flag_36c: u8,
    /// +0x370 the proximity watcher.
    pub prox: ProxWatcher,
    /// +0x38c the directive.
    pub op: GotoOp,
    /// +0x3fc (`FreeFlightBehavior`).
    pub flag_3fc: u8,
}

/// Three dwords of a goto op's `words_20` block as a vector.
fn op_vector(op: &GotoOp, at: usize) -> [f32; 3] {
    [
        f32::from_bits(op.words_20[at]),
        f32::from_bits(op.words_20[at + 1]),
        f32::from_bits(op.words_20[at + 2]),
    ]
}

/// `GotoClampUnit`: below -1 (or NaN) -1, at or above 1 1.
fn clamp_unit(value: f32) -> f32 {
    if !(value > -1.0) {
        return -1.0;
    }
    if !(value < 1.0) {
        return 1.0;
    }
    value
}

/// `(x*x + y*y) + z*z`, unrounded.
fn len_sq_xyz(v: Vector) -> f64 {
    (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)
}

impl GotoBehavior {
    /// `GotoBehavior::GotoBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            free_flight: false,
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            spline_idea: SplineMoveIdea::new(),
            steer: Vector::ZERO,
            field_2e4: 0.0,
            flag_2e8: 0,
            flag_2e9: 1,
            flag_2ea: 0,
            flag_2eb: 0,
            flag_2ec: 0,
            field_2f0: 1.5,
            kind: 3,
            lead: Vector::ZERO,
            destination: Vector::ZERO,
            watcher_310: None,
            spline: None,
            range: 200.0,
            field_35c: -1.0,
            throttle_360: 1.0,
            flag_364: 0,
            flag_365: 1,
            flag_366: 0,
            flag_367: 0,
            flag_368: 0,
            flag_369: 0,
            flag_36a: 0,
            flag_36b: 0,
            flag_36c: 1,
            prox: ProxWatcher::default(),
            op: GotoOp::default(),
            flag_3fc: 0,
        }
    }

    /// `FreeFlightBehavior::FreeFlightBehavior(TypeId)`: `Flag2eb` set.
    #[must_use]
    pub fn new_free_flight(id: i32) -> Self {
        let mut b = Self::new(id);
        b.free_flight = true;
        b.flag_2eb = 1;
        b
    }

    fn is_player<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> bool {
        cx.own().is_some_and(|v| v.is_player)
    }

    /// The finish sequence the update repeats: disengage requested, the
    /// move idea's cruise dropped, slot 4, the directive flag cleared.
    fn finish<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.base.flags |= 0x1;
        self.flag_368 = 0;
        Behavior::<W>::disengage(self, cx);
        self.base.flags &= 0xfd;
    }

    /// `GotoBehavior::Slot4`.
    fn goto_disengage<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.face.commit(cx);
        if Self::is_player(cx) && self.kind == 0 {
            self.move_idea.set_field_44(2);
        }
        self.move_idea.commit(cx);
        cx.set_strafe_dir(StrafeDir::None);
        self.base.disengage(cx.state);
    }

    /// `GotoBehavior::Update`, slot 7.
    #[allow(clippy::too_many_lines)]
    fn goto_update<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if self.base.flags & 0x1 != 0 {
            self.base.result = 2;
            return 2;
        }
        let Some(own) = cx.own() else {
            self.base.result = 3;
            return 3;
        };
        if self.base.flags & 0x2 == 0 {
            self.base.result = 3;
            return 3;
        }
        let mut result = 3;
        self.lead = own.center;
        let is_player = own.is_player;
        let mut throttle = self.throttle_360;
        let mut clearance = 0.0_f32;
        if is_player {
            throttle = if self.flag_2e8 != 0 && self.flag_36b == 0 {
                1.0
            } else {
                self.field_2e4
            };
        }
        let dt = self.base.tick_delta;

        if self.kind == 1 {
            clearance = r(signed_radial_clearance_to_point(&own, self.destination));
            if self.flag_2ea != 0 {
                let dir = neg(column2(&own.orientation));
                let scaled_x = r(e(dir.x) * 1750.0);
                let scaled_y = r(e(dir.y) * 1750.0);
                let ahead = Vector::new(
                    r(e(scaled_x) + e(own.center.x)),
                    r(e(scaled_y) + e(own.center.y)),
                    r(e(dir.z) * 1750.0 + e(own.center.z)),
                );
                if len_sq_xyz(self.steer) > 0.0 {
                    self.face.set_axis(self.steer);
                } else {
                    self.face.set_position(ahead);
                }
                self.move_idea.set_field_44(2);
                self.move_idea.set_direction(dir);
                self.flag_366 = 1;
                if clearance < self.range {
                    self.finish(cx);
                    cx.set_directive_result(2, 0);
                    self.base.result = 2;
                    return 2;
                }
            } else {
                self.face.set_position(self.destination);
                self.move_idea.set_position(cx, self.destination);
            }
            self.lead = self.destination;
        } else if self.kind == 0 {
            let Some(target) = cx.view_of(self.watcher_310) else {
                self.finish_without_drop(cx);
                cx.set_directive_result(2, 2);
                if is_player {
                    cx.world.command(cx.ship, slot_6c(3, 0));
                }
                self.finish(cx);
                cx.set_directive_result(2, 2);
                self.base.result = result;
                return result;
            };
            if is_player && len_sq_xyz(target.velocity) > 25.0 {
                cx.world.command(cx.ship, slot_6c(2, 0));
                self.finish(cx);
                cx.set_directive_result(2, 2);
                self.base.result = result;
                return result;
            }
            self.lead = iface_position_or_zero(Some(&target));
            clearance = r(get_intercept_clearance_of_iface(&own, &target));
            if self.flag_2ea != 0 {
                let dir = neg(column2(&own.orientation));
                self.face.set_axis(self.steer);
                self.move_idea.set_direction(dir);
                if clearance < self.range {
                    self.finish(cx);
                    cx.set_directive_result(2, 0);
                    self.base.result = 2;
                    return 2;
                }
            } else {
                self.move_idea.set_watch_object(cx, self.watcher_310);
                self.face.set_watch_object(self.watcher_310);
            }
        }

        let lead = self.lead;
        self.prox.recompute_zone_flags(&*cx, lead);
        self.prox.notify_zone_change(cx, lead);

        if self.kind == 2 {
            if let Some(spline) = &self.spline {
                self.lead = Vector::new(spline.p2[0], spline.p2[1], spline.p2[2]);
            }
            self.spline_idea.set_bank_blend_cap(throttle);
            if self.prox.watched.is_some() && cx.view_of(self.prox.watched).is_some() {
                clearance = r(self.prox.speed_cap(&*cx, 1, self.throttle_360));
                let own_speed =
                    with_own_motion(&*cx, |m| speed(m, throttle, ThrustEquipType::Engine));
                if own_speed > clearance {
                    self.spline_idea.set_throttle_scale(&*cx, clearance);
                }
            }
            let spline_result = Idea::<W>::update(&mut self.spline_idea, cx, dt);
            if spline_result == 0 {
                self.base.result = 1;
                return 1;
            }
            if spline_result != 1 {
                result = 3;
            } else {
                if self.prox_alive(cx) && self.prox.zone_flags & 0x2 == 0 && self.flag_366 == 0 {
                    self.base.result = 1;
                    return 1;
                }
                result = 2;
            }
        } else if self.kind == 0 || self.kind == 1 {
            self.move_idea
                .set_field_44(if self.flag_366 != 0 { 2 } else { 0 });
            if self.flag_365 != 0 {
                self.flag_364 = u8::from(clearance > 1750.0);
                self.flag_365 = 0;
            }
            let mut cruise = if is_player {
                if self.flag_2e8 != 0 {
                    self.flag_364
                } else {
                    self.flag_2e8
                }
            } else {
                self.flag_364
            };
            if cruise != 0 {
                cruise = u8::from(cx.state.pathfind.is_cruise_unobstructed());
            }
            if self.flag_36a != 0 {
                cruise = 1;
            }
            if self.flag_36b != 0 {
                cruise = 0;
            }

            let (valid, envelope) = formation_follower_envelope(&*cx);
            if valid {
                self.move_idea.set_angle_38(PI_F);
                if envelope.throttle < throttle {
                    throttle = envelope.throttle;
                }
                self.move_idea.set_scale_40(throttle);
                self.face.set_throttle_scale(&*cx, envelope.angular_speed);
                if !envelope.flag_10 {
                    cruise = 0;
                }
            } else {
                self.face.set_scale_94(1.0);
            }

            let prox_target = cx.view_of(self.prox.watched);
            let target_cruise = prox_target
                .as_ref()
                .is_some_and(|t| t.cruise.is_some_and(|c| c.0));
            if self.flag_36a == 0
                && prox_target.is_some()
                && !target_cruise
                && !self
                    .prox
                    .watched
                    .is_some_and(|id| own.formation_followers.contains(&id))
            {
                cruise = 0;
            }

            if cruise != self.flag_368 {
                if cruise != 0 {
                    self.move_idea.set_mode(cx, 2);
                    self.flag_368 = 1;
                } else {
                    self.move_idea.set_range_3c(self.range);
                    self.move_idea.set_mode(cx, 0);
                    self.flag_368 = 0;
                }
            }

            if prox_target.is_some() {
                let selector = if self.flag_368 != 0 { 4 } else { 1 };
                clearance = r(self.prox.speed_cap(&*cx, selector, self.throttle_360));
                let query = if self.flag_368 != 0 && target_cruise {
                    ThrustEquipType::Cruise
                } else {
                    ThrustEquipType::Engine
                };
                let own_speed = with_own_motion(&*cx, |m| speed(m, throttle, query));
                if own_speed > clearance {
                    throttle = with_own_motion(&*cx, |m| projected_throttle(m, clearance, query));
                }
            }

            let face_result = self.face.update(cx);

            let mut index = 0;
            let mut run_move = true;
            if is_player {
                if self.flag_2ec != 0 {
                    cx.set_throttle(-1.0, 0);
                    run_move = false;
                } else if self.flag_2e9 == 0 {
                    cx.set_engine_active(false);
                    run_move = false;
                }
            }
            if run_move {
                self.move_idea.set_scale_40(throttle);
                index = self.move_idea.update(cx, dt);
            }

            if index == 0 {
                self.base.result = 1;
                return 1;
            }
            if index == 1 {
                let mut face_done = face_result == 1;
                if self.flag_36c == 0 {
                    face_done = true;
                }
                if is_player && cruise == 0 && !face_done {
                    self.base.result = 1;
                    return 1;
                }
                if self.prox_alive(cx) && self.prox.zone_flags & 0x2 == 0 && self.flag_366 == 0 {
                    self.base.result = 1;
                    return 1;
                }
                result = 2;
            } else if (0..=3).contains(&index) {
                result = 3;
            }
        }

        self.finish(cx);
        if result == 2 {
            cx.set_directive_result(2, 0);
        } else {
            cx.set_directive_result(2, 2);
        }
        self.base.result = result;
        result
    }

    /// The first finish of the "watched object gone" path (the reference
    /// does not drop the move idea's cruise there).
    fn finish_without_drop<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.base.flags |= 0x1;
        Behavior::<W>::disengage(self, cx);
        self.base.flags &= 0xfd;
    }

    fn prox_alive<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> bool {
        cx.view_of(self.prox.watched).is_some()
    }

    /// `GotoBehavior::Slot10`.
    fn goto_accept<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, op: &GotoOp) -> bool {
        self.base.flags &= 0xfe;
        self.op = op.clone();
        self.kind = op.kind;
        match op.kind {
            0 => {
                let id = op.field_1c;
                let id = u32::from_ne_bytes(id.to_ne_bytes());
                self.watcher_310 = cx.view_of(Some(id)).map(|v| v.id);
                self.move_idea.set_range_3c(self.range);
                self.move_idea.set_watch_object(cx, self.watcher_310);
                self.face.set_watch_object(self.watcher_310);
            }
            1 => {
                self.destination = op.destination;
                self.move_idea.set_position(cx, self.destination);
                self.face.set_position(self.destination);
            }
            2 => {
                let mut spline = CrSplineSegment::new(
                    op_vector(op, 6),
                    op_vector(op, 0),
                    op_vector(op, 3),
                    op_vector(op, 9),
                );
                spline.calculate_arclength();
                self.spline_idea.set_spline(&*cx, spline);
                self.spline_idea.set_wide_mode(cx, op.cruise);
                self.lead = Vector::new(spline.p2[0], spline.p2[1], spline.p2[2]);
                self.spline = Some(spline);
            }
            _ => {
                self.base.flags &= 0xfd;
                return false;
            }
        }
        self.range = op.range;
        self.move_idea.set_range_3c(self.range);
        self.field_35c = op.field_54;
        self.throttle_360 = if !(e(op.field_54) <= 1.0e-5) {
            let v = op.field_54;
            with_own_motion(&*cx, |m| projected_throttle(m, v, ThrustEquipType::Engine))
        } else {
            1.0
        };
        self.flag_367 = op.field_59;
        self.move_idea.set_enabled(self.flag_367);
        self.flag_366 = op.field_58;
        self.move_idea
            .set_field_44(if op.field_58 != 0 { 2 } else { 0 });
        self.spline_idea
            .set_field_158(if op.field_58 != 0 { 2 } else { 0 });
        cx.set_cruise_lock(u8::from(op.field_6c == 0), 3);
        self.flag_36a = op.cruise;
        self.flag_36b = op.no_cruise;
        let (target, near, far, ratio) = if op.field_5c != 0 {
            (
                Some(op.field_5c),
                op.field_60,
                op.field_64,
                f32::from_bits(op.field_68),
            )
        } else {
            (None, 0.0, 0.0, 0.0)
        };
        self.prox.init(&*cx, target, near, far, ratio);
        self.base.flags |= 0x2;
        self.flag_2ea = 0;
        true
    }
}

/// `IObjAffect::slot_6c(a, b)` (slot 27).
fn slot_6c<H>(a: i64, b: i64) -> AiCommand<H> {
    AiCommand::Slot {
        slot: 27,
        ints: vec![a, b],
        floats: Vec::new(),
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for GotoBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    /// (1) `SetContext`: `Flag367` set; the ideas bind in [`Self::attach`].
    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let ok = self.base.bind(owner_id);
        if ok {
            self.flag_367 = 1;
            self.move_idea.set_enabled(self.flag_367);
        }
        ok
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.move_idea.set_context(&*cx);
        self.move_idea.set_enabled(self.flag_367);
        self.face.set_context(&*cx);
        self.spline_idea.set_context(cx);
    }

    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        if self.free_flight {
            self.base.flags |= 0x2;
            self.flag_365 = 1;
            self.flag_3fc = 0;
            self.move_idea.set_angle_38(PI_F);
            self.range = 0.0;
            self.move_idea.set_range_3c(0.0);
            self.move_idea.set_enabled(0);
            self.move_idea.set_field_44(2);
            let avoidance = u8::from(cx.state.thrust.auto_avoidance);
            cx.set_cruise_lock(avoidance, 3);
            self.kind = 1;
            self.flag_2ea = 1;
            self.steer = Vector::ZERO;
            let own = cx.own();
            if let Some(t) = own.as_ref().and_then(|v| v.throttle) {
                self.field_2e4 = t;
            }
            if let Some(c) = own.as_ref().and_then(|v| v.cruise) {
                self.flag_2e8 = u8::from(c.0);
            }
            if self.field_2e4 < 0.0 {
                self.field_2e4 = 1.0;
                self.flag_2ec = 1;
            } else {
                self.flag_2ec = 0;
            }
            self.destination = Vector::new(1_000_000.0, 1_000_000.0, 1_000_000.0);
            return;
        }
        self.flag_369 = 0;
        let cruise = cx.own().and_then(|v| v.cruise);
        if let Some(c) = cruise {
            self.flag_368 = u8::from(c.0);
        }
        let mode = if self.flag_368 != 0 && cruise.is_some() {
            2
        } else {
            0
        };
        self.move_idea.set_mode(cx, mode);
        self.flag_365 = 1;
        self.flag_36c = 1;
        self.move_idea.set_angle_38(PI_F);
        self.field_2e4 = self.throttle_360;
        self.flag_2ea = 0;
        self.flag_2ec = 0;
        cx.set_cruise_lock(1, 3);
        if Self::is_player(cx) {
            Behavior::<W>::set_cruise(self, cx, true);
        }
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        if self.free_flight {
            self.base.disengage(cx.state);
            return;
        }
        self.goto_disengage(cx);
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = 2;
        if self.base.flags & 0x2 != 0 {
            result = if self.base.flag29 != 0 { 3 } else { 1 };
        } else if self.base.flag29 != 0 {
            result = 3;
        }
        self.base.mode = result;
        result
    }

    fn evaluate(&mut self, cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        if self.free_flight {
            return EvaluateResult::Blocked;
        }
        let Some(own) = cx.own() else {
            return EvaluateResult::Blocked;
        };
        let Some(target) = cx.view_of(own.target) else {
            return EvaluateResult::Blocked;
        };
        // `is_using_tradelane` on both: the AI world has no trade-lane
        // state, so neither is in a lane.
        let kind = target.type_flags;
        if kind & 0x7f_0000 != 0 && matches!(target.behavior_id, 0xe | 0x11 | 0x8) {
            return EvaluateResult::Skip;
        }
        if kind & 0x1000_0000 != 0 {
            return EvaluateResult::Blocked;
        }
        let v = target.velocity;
        let speed_sq = (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z);
        if speed_sq > 25.0 {
            EvaluateResult::Skip
        } else {
            EvaluateResult::Blocked
        }
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        self.goto_update(cx)
    }

    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        if self.free_flight {
            self.base.flags = (self.base.flags & 0xfe) | 0x2;
            self.steer = Vector::ZERO;
            self.field_2e4 = 0.0;
            self.flag_2e8 = 0;
            return true;
        }
        match op {
            DirectiveOp::Goto(g) => self.goto_accept(cx, g),
            _ => {
                // The op is read as a goto op whatever its type; another
                // op's kind word is not 0 to 2 here.
                self.base.flags &= 0xfd;
                false
            }
        }
    }

    fn set_direction(&mut self, cx: &mut Cx<'_, W>, dir: Vector) {
        let proceed = match self.kind {
            1 => true,
            0 => len_sq_xyz(dir) > 0.0,
            _ => false,
        };
        if !proceed {
            self.flag_2ea = 0;
            return;
        }
        self.flag_2ea = 1;
        let mut x = -dir.y;
        let mut y = -dir.x;
        let factor = e(cx.state.turn_sensitivity);
        if factor > 0.0 {
            let inv = 1.0 / factor;
            x = r(inv * e(x));
            y = r(inv * e(y));
        }
        self.steer = Vector::new(clamp_unit(x), clamp_unit(y), -dir.z);
    }

    fn set_throttle(&mut self, _cx: &mut Cx<'_, W>, throttle: f32) {
        self.flag_2eb = 1;
        self.field_2e4 = throttle;
        self.flag_2e9 = 1;
    }

    fn set_cruise(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        if on {
            self.flag_2e8 = 1;
            self.field_2e4 = self.throttle_360;
            self.flag_2e9 = 1;
            return;
        }
        if self.flag_2e8 != 0 {
            self.field_2e4 = self.throttle_360;
        }
        cx.engage_cruise(false);
        self.flag_2e8 = 0;
        self.flag_2e9 = 1;
    }

    fn engage_engine(&mut self, _cx: &mut Cx<'_, W>, on: bool) {
        if self.free_flight {
            self.flag_2e9 = u8::from(on);
            self.flag_2e8 = 0;
        }
    }

    fn brake_reverse(&mut self, _cx: &mut Cx<'_, W>, on: bool) {
        self.flag_2ec = u8::from(on);
    }

    fn object_left(&mut self, _cx: &mut Cx<'_, W>, id: u32) {
        if self.watcher_310 == Some(id) {
            self.watcher_310 = None;
        }
        if self.prox.watched == Some(id) {
            self.prox.watched = None;
        }
    }

    fn target(&self) -> Option<u32> {
        self.watcher_310
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clamp_and_zone_codes() {
        assert_eq!(clamp_unit(f32::NAN), -1.0);
        assert_eq!(clamp_unit(2.0), 1.0);
        assert_eq!(clamp_unit(0.25), 0.25);
        let mut p = ProxWatcher {
            watched: Some(5),
            ..ProxWatcher::default()
        };
        p.saved_flags = 4;
        p.zone_flags = 6;
        assert_eq!(p.zone_change(), Some(1));
        p.saved_flags = 0;
        p.zone_flags = 4;
        assert_eq!(p.zone_change(), Some(2));
        p.saved_flags = 4;
        p.zone_flags = 0;
        assert_eq!(p.zone_change(), Some(3));
        p.saved_flags = 0;
        assert_eq!(p.zone_change(), None);
    }
}
