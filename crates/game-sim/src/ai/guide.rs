//! `GuideBehavior` (behaviour 14, `Common/common_guidebehavior.cpp`, class
//! in `include/common_guidebehavior.h`): lead a ship to a destination.
//!
//! The guide hands the flying to two of the manager's behaviours: the
//! follow behaviour (with the guide op's follow part) while the guided ship
//! makes its own way, and the goto behaviour (with its goto part) while the
//! guide leads. Every sampling period it measures how much closer the
//! guided ship came to the destination and whether it points there, and
//! switches between the two; each switch would be a guide event to the
//! content callback (`lead_object_event`, queued for after the tick). It ends once both it is at the
//! destination and the guided ship is close.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, FollowOp, GotoOp, ids};
use super::content::CombatEvent;
use super::follower::FollowBehavior;
use super::fp::{e, normalize3, r, sub};
use super::gun_convergence::FormationState;
use super::intercept::{
    get_intercept_clearance_of_iface, iface_position_or_zero, is_within_intercept_range_of_iface,
    signed_radial_clearance_to_point,
};
use super::raycast::clamped_dot3;
use super::world::ObjectView;

/// `GuideBehavior` (0x124 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct GuideBehavior {
    base: BehaviorBase,
    /// +0x2c the guided ship (a watcher).
    pub guided: Option<u32>,
    /// +0x34 the poll's range (the op's `field_b8`).
    pub field_34: f32,
    /// +0x38 start by following (1; the op's `field_a0`).
    pub flag_38: u8,
    /// +0x3c the leading half's clearance (the goto part's `field_60`).
    pub field_3c: f32,
    /// +0x40 the goto part's `field_64` (bits).
    pub field_40: u32,
    /// +0x44 the following half's clearance.
    pub field_44: f32,
    /// +0x48 the destination.
    pub destination: Vector,
    /// +0x54 the arrival range.
    pub field_54: f32,
    /// +0x58 the ground the guided ship closed in the last sample.
    pub field_58: f32,
    /// +0x5c how far off course the guided ship points (radians).
    pub field_5c: f32,
    /// +0x60 / +0x64 the manager's goto and follow behaviours were
    /// captured.
    pub delegates: bool,
    /// +0x68 the goto part.
    pub goto_op: GotoOp,
    /// +0xd8 the follow part.
    pub follow_op: FollowOp,
    /// +0xfc the goto part's object (a watcher).
    pub goto_object: Option<u32>,
    /// +0x104 the sampling period (10).
    pub period: f32,
    /// +0x108 the time since the last sample.
    pub elapsed: f32,
    /// +0x10c the guided ship's distance at the last sample.
    pub last_distance: f32,
    /// +0x110 the op's `field_ac`.
    pub field_110: f32,
    /// +0x114 the falling-behind threshold (-10).
    pub low: f32,
    /// +0x118 the closing threshold (10).
    pub high: f32,
    /// +0x11c the course angle (π/8).
    pub angle: f32,
    /// +0x120 the follow behaviour drives.
    pub following: u8,
}

impl GuideBehavior {
    /// `GuideBehavior::GuideBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            guided: None,
            field_34: 0.0,
            flag_38: 1,
            field_3c: 0.0,
            field_40: 0,
            field_44: 0.0,
            destination: Vector::ZERO,
            field_54: 0.0,
            field_58: 0.0,
            field_5c: 0.0,
            delegates: false,
            goto_op: GotoOp::default(),
            follow_op: FollowOp::default(),
            goto_object: None,
            period: 10.0,
            elapsed: 0.0,
            last_distance: 0.0,
            field_110: 0.0,
            low: -10.0,
            high: 10.0,
            angle: f32::from_bits(0x3ec9_0fdb),
            following: 0,
        }
    }

    fn engage_follow<W: AiEnv + ?Sized>(&self, cx: &mut Cx<'_, W>) {
        let op = DirectiveOp::Follow(self.follow_op.clone());
        cx.with_peer(ids::FOLLOW, |b, cx| {
            b.accept_directive(cx, &op);
            b.engage(cx);
        });
    }

    fn engage_goto<W: AiEnv + ?Sized>(&self, cx: &mut Cx<'_, W>) {
        let op = DirectiveOp::Goto(self.goto_op.clone());
        cx.with_peer(ids::GOTO, |b, cx| {
            b.accept_directive(cx, &op);
            b.engage(cx);
        });
    }

    fn tick_peer<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, index: i32, dt: f32) -> i32 {
        cx.with_peer(index, |b, cx| {
            b.base_mut().tick_delta = dt;
            b.update(cx)
        })
        .unwrap_or(3)
    }

    /// The body of `Update` after the guards.
    fn run<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        a: &ObjectView,
        own: &ObjectView,
    ) -> i32 {
        match self.goto_op.kind {
            0 => {
                let Some(b) = cx.view_of(self.goto_object) else {
                    self.base.flags &= 0xfd;
                    cx.set_directive_result(ids::GUIDE, 1);
                    return 2;
                };
                self.destination = iface_position_or_zero(Some(&b));
            }
            1 => self.destination = self.goto_op.destination,
            _ => {}
        }
        self.field_54 = self.goto_op.range;
        let a_pos = iface_position_or_zero(Some(a));
        let clearance = r(get_intercept_clearance_of_iface(own, a));
        let radial = r(signed_radial_clearance_to_point(own, self.destination));
        let dist = {
            let dx = e(a_pos.x) - e(self.destination.x);
            let dy = e(a_pos.y) - e(self.destination.y);
            let dz = e(a_pos.z) - e(self.destination.z);
            r(((dx * dx + dz * dz) + dy * dy).sqrt())
        };
        let inside_radial = dist < radial;
        let mut leader_is_self = false;
        if a.behavior_id == ids::FORMATION
            && let Some(leader) = cx.view_of(a.formation_leader)
            && leader.id == own.id
        {
            self.following = 0;
            leader_is_self = true;
        }
        if FormationState::is_follower(&*cx.world, cx.ship, a.id) {
            self.following = 0;
        }
        let mut sampled = false;
        if self.elapsed > self.period {
            self.field_58 = r(e(self.last_distance) - e(dist));
            self.elapsed = 0.0;
            let delta = sub(self.destination, a_pos);
            let m = &a.orientation.m;
            let neg_forward = Vector::new(-m[0][2], -m[1][2], -m[2][2]);
            let norm = normalize3(delta);
            self.field_5c = r(clamped_dot3(neg_forward, norm).acos());
            self.last_distance = dist;
            sampled = true;
        } else {
            self.elapsed = r(e(self.elapsed) + e(self.base.tick_delta));
        }
        // Each guide event goes to `NotifyManagerGuideEvent`, which
        // reports it to the content callback while the ship is in an
        // attack state and guides (`FireGuideEvent`).
        let (own_id, watched, point) = (own.id, a.id, self.destination);
        let fire = move |cx: &mut Cx<'_, W>, progress: i32| {
            let event = CombatEvent::Lead {
                own: own_id,
                watched,
                progress,
                value: clearance,
                position: point,
            };
            let _ = cx.notify_content(&[ids::GUIDE], event);
        };
        let dt = self.base.tick_delta;
        let mut skip_radial = false;
        if self.following != 0 {
            if sampled {
                if self.field_58 < self.low {
                    if !inside_radial {
                        fire(cx, 8);
                    }
                    self.following = 0;
                } else if self.field_58 > self.high {
                    if self.field_5c > self.angle {
                        if !inside_radial {
                            fire(cx, 6);
                        }
                        self.following = 0;
                    } else {
                        fire(cx, 5);
                    }
                } else if clearance < self.field_44 {
                    if !inside_radial {
                        fire(cx, 7);
                    }
                    self.following = 0;
                }
            }
            if self.following != 0 {
                Self::tick_peer(cx, ids::FOLLOW, dt);
            } else {
                cx.with_peer(ids::FOLLOW, |b, cx| b.disengage(cx));
                self.engage_goto(cx);
            }
        } else {
            if sampled {
                if leader_is_self {
                    fire(cx, 4);
                } else if self.field_58 < self.low && !inside_radial {
                    fire(cx, 3);
                } else {
                    let far_ahead = self.field_58 > self.high;
                    let inside_3c = clearance < self.field_3c;
                    let c0 = !(clearance >= self.field_3c);
                    if far_ahead {
                        if inside_3c {
                            if self.field_5c <= self.angle {
                                fire(cx, 1);
                                self.following = 1;
                            }
                        } else if inside_radial {
                            fire(cx, 4);
                        }
                    } else if !c0 && !inside_radial {
                        fire(cx, 2);
                        self.following = 1;
                    }
                }
            }
            if self.following != 0 {
                cx.with_peer(ids::GOTO, |b, cx| b.disengage(cx));
                self.engage_follow(cx);
            } else if Self::tick_peer(cx, ids::GOTO, dt) == 2 {
                skip_radial = true;
            }
        }
        let own_now = cx.own().unwrap_or_else(|| own.clone());
        if !skip_radial
            && !(signed_radial_clearance_to_point(&own_now, self.destination) <= e(self.field_54))
        {
            return 1;
        }
        let a_now = cx.view_of(Some(a.id)).unwrap_or_else(|| a.clone());
        if !(get_intercept_clearance_of_iface(&own_now, &a_now) <= e(self.goto_op.field_60)) {
            return 1;
        }
        self.base.flags &= 0xfd;
        cx.set_directive_result(ids::GUIDE, 0);
        2
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for GuideBehavior {
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

    /// Slot 3: both children get their op (the follow behaviour set up as
    /// for a player: its cruise latch, the match throttle's flag 39 and
    /// the move idea's mode 2) and engage; the first sample's distance.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.delegates = true;
        self.engage_goto(cx);
        let op = DirectiveOp::Follow(self.follow_op.clone());
        cx.with_peer(ids::FOLLOW, |b, cx| {
            b.accept_directive(cx, &op);
            if let Some(f) = b
                .as_any_mut()
                .and_then(|a| a.downcast_mut::<FollowBehavior>())
            {
                f.f.flag_28c = 1;
                f.f.match_throttle.flag_39 = 1;
                f.f.move_idea.set_field_44(2);
            }
            b.engage(cx);
        });
        self.destination = self.goto_op.destination;
        self.field_54 = self.goto_op.range;
        self.elapsed = 0.0;
        let pos = iface_position_or_zero(cx.view_of(self.guided).as_ref());
        let dx = e(pos.x) - e(self.destination.x);
        let dy = e(pos.y) - e(self.destination.y);
        let dz = e(pos.z) - e(self.destination.z);
        self.following = self.flag_38;
        self.field_58 = 0.0;
        self.field_5c = 0.0;
        self.last_distance = r(((dx * dx + dz * dz) + dy * dy).sqrt());
    }

    /// Slot 4: both children disengage and lose their directive flag.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        if self.delegates {
            for index in [ids::GOTO, ids::FOLLOW] {
                cx.with_peer(index, |b, cx| {
                    b.disengage(cx);
                    // `Behavior_ReleaseDirectiveTarget`.
                    let base = b.base_mut();
                    base.flags = (base.flags & 0xfd) | 0x01;
                });
            }
        }
        self.base.disengage(cx.state);
    }

    /// Slot 5: with the directive, 1 within the op's range of the guided
    /// ship, else 0; a guided ship that is gone ends the directive.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        if self.base.flags & 0x2 != 0 {
            if let (Some(lead), Some(own)) = (cx.view_of(self.guided), cx.own()) {
                let mode = if is_within_intercept_range_of_iface(&own, &lead, self.field_34) {
                    1
                } else {
                    0
                };
                self.base.mode = if self.base.flag29 != 0 { 3 } else { mode };
                return self.base.mode;
            }
            cx.set_directive_result(ids::GUIDE, 1);
        }
        self.base.mode = if self.base.flag29 != 0 { 3 } else { 2 };
        self.base.mode
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let Some(a) = cx.view_of(self.guided) else {
            self.base.flags &= 0xfd;
            self.base.result = 3;
            return 3;
        };
        let Some(own) = cx.own().filter(|_| self.base.bound) else {
            self.base.result = 3;
            return 3;
        };
        let result = self.run(cx, &a, &own);
        self.base.result = result;
        result
    }

    /// Slot 10: the guide op; the guided ship and the goto part's object
    /// looked up, both children given their part.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let DirectiveOp::Guide(op) = op else {
            self.base.flags &= 0xfd;
            return false;
        };
        self.delegates = true;
        self.goto_op = op.goto.clone();
        self.follow_op = op.follow.clone();
        let mut ok = true;
        let object = u32::from_ne_bytes(self.goto_op.field_1c.to_ne_bytes());
        self.goto_object = cx.view_of((object != 0).then_some(object)).map(|v| v.id);
        self.period = op.field_a4;
        self.angle = op.field_a8;
        self.field_110 = op.field_ac as f32;
        self.high = op.field_b0;
        self.low = op.field_b4;
        let guided = self.goto_op.field_5c;
        if guided != 0 {
            self.guided = cx.view_of(Some(guided)).map(|v| v.id);
            if self.guided.is_some() {
                self.field_3c = self.goto_op.field_60;
                self.field_40 = self.goto_op.field_64.to_bits();
                let v = self.follow_op.offset;
                let len = ((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt();
                self.field_44 = if len > 0.0 {
                    r(len + 100.0)
                } else {
                    self.follow_op.distance
                };
                let goto = DirectiveOp::Goto(self.goto_op.clone());
                if cx.with_peer(ids::GOTO, |b, cx| b.accept_directive(cx, &goto)) != Some(true) {
                    ok = false;
                }
                let follow = DirectiveOp::Follow(self.follow_op.clone());
                if cx.with_peer(ids::FOLLOW, |b, cx| b.accept_directive(cx, &follow)) != Some(true)
                {
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        self.field_34 = op.field_b8;
        self.flag_38 = op.field_a0;
        self.base.flags = (self.base.flags & 0xfd) | if ok { 0x2 } else { 0 };
        ok
    }

    /// Slot 21: the guided ship left.
    fn object_left(&mut self, _cx: &mut Cx<'_, W>, id: u32) {
        if self.guided == Some(id) {
            self.guided = None;
        }
    }

    /// Slot 24: the guided ship.
    fn target(&self) -> Option<u32> {
        self.guided
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        let g = GuideBehavior::new(14);
        assert_eq!((g.period, g.low, g.high, g.flag_38), (10.0, -10.0, 10.0, 1));
        assert_eq!(g.angle.to_bits(), 0x3ec9_0fdb);
    }
}
