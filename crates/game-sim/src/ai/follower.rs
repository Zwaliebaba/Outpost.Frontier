//! The follower behaviours: `BaseFollowerBehavior`
//! (`Common/common_basefollowerbehavior.cpp`, class in
//! `include/common_behavior.h`), its two subclasses `FormationBehavior`
//! (behaviour 10, `common_formationbehavior.cpp`) and `FollowBehavior`
//! (behaviour 17, `common_followbehavior.cpp`).
//!
//! The base owns the leader watcher, five ideas (match throttle, face,
//! move, break away, afterburner) and the per-tick body (slot 7): it takes
//! the leader from the system manager's formation block, checks it for a
//! player, drives the manager's goto behaviour for a player whose leader
//! is close and slow, hands a leader's trade-lane order to the manager's
//! dock behaviour, and otherwise runs either the subclass's own work (slot
//! 27) or the formation exit of the personality. The formation behaviour
//! keeps the ship at its slot in the leader's frame; the follow behaviour
//! keeps it at the follow directive's offset.
//!
//! The dock delegate takes the order as a `DirectiveDockOp` the port does
//! not carry yet (`DirectiveOp::Other(13)`), and the slot-28 offset it
//! would be given (`DockBehavior::SetDestPointOffset`) is not passed on:
//! the dock behaviour is not part of this module. No recorded scenario
//! has a trade-lane order.

use neuron_core::math::{Matrix, Vector, transform_matrix};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, EvaluateResult, GotoOp};
use super::dodge::BreakAwayIdea;
use super::face_idea::FaceIdea;
use super::fp::{add, column2, e, len_sq_xyz, neg, r, sub};
use super::idea::{AfterburnerIdea, Idea};
use super::intercept::{
    get_intercept_clearance_of_iface, iface_vector_magnitude, is_within_intercept_range_of_iface,
    negated_iface_direction, object_type_or_zero,
};
use super::match_throttle::MatchThrottle2Idea;
use super::move_idea::MoveIdea;
use super::timer_table::slot;
use super::world::{AiCommand, ObjectView, StrafeDir};

/// Behaviour ids a leader may not run (`get_behavior_id` 0x11 follow, 8
/// launch).
const BARRED_LEADER_BEHAVIORS: [i32; 2] = [0x11, 8];

/// `BaseFollowerBehavior`'s own block (`+0x2c..+0x2a0`).
#[derive(Clone, Debug, PartialEq)]
pub struct FollowerBase {
    /// The `Behavior` block.
    pub base: BehaviorBase,
    /// +0x2c the leader (a watcher: an object that is gone reads as none).
    pub lead: Option<u32>,
    /// +0x34 the match-throttle idea.
    pub match_throttle: MatchThrottle2Idea,
    /// +0x90 the face idea.
    pub face: FaceIdea,
    /// +0x150 the move idea.
    pub move_idea: MoveIdea,
    /// +0x1c8 the break-away idea.
    pub break_away: BreakAwayIdea,
    /// +0x264 the afterburner idea.
    pub afterburner: AfterburnerIdea,
    /// +0x278 / +0x27c the manager's dock and goto behaviours were
    /// captured (slot 3) and not yet released (slot 4).
    pub delegates: bool,
    /// +0x280 the own ship left a trade lane.
    pub left_lane: u8,
    /// +0x281 a trade-lane order was handed to `go_tradelane`.
    pub lane_order: u8,
    /// +0x282 the dock delegate runs.
    pub dock_running: u8,
    /// +0x283 the owner is a player (latched by slot 3).
    pub player: u8,
    /// +0x284 the goto delegate runs.
    pub goto_running: u8,
    /// +0x285 the parked order was consumed.
    pub consumed: u8,
    /// +0x288 zeroed by slot 3.
    pub field_288: i32,
    /// +0x28c the formation behaviour's player latch.
    pub flag_28c: u8,
    /// +0x290 the formation or follow offset.
    pub offset: Vector,
    /// +0x29c zeroed by slot 4.
    pub field_29c: i32,
}

impl FollowerBase {
    /// `BaseFollowerBehavior::BaseFollowerBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            lead: None,
            match_throttle: MatchThrottle2Idea::new(),
            face: FaceIdea::new(),
            move_idea: MoveIdea::new(),
            break_away: BreakAwayIdea::new(),
            afterburner: AfterburnerIdea::default(),
            delegates: false,
            left_lane: 0,
            lane_order: 0,
            dock_running: 0,
            player: 0,
            goto_running: 0,
            consumed: 0,
            field_288: 0,
            flag_28c: 0,
            offset: Vector::ZERO,
            field_29c: 0,
        }
    }

    /// The leader while it lives.
    fn lead_view<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> Option<ObjectView> {
        cx.view_of(self.lead)
    }

    /// The bind half of slot 1 (`SetContext`): the watcher cleared, the
    /// two ideas without a world bound.
    fn bind<W: AiEnv + ?Sized>(&mut self, owner_id: Option<u32>) -> bool {
        if !self.base.bind(owner_id) {
            return false;
        }
        self.lead = None;
        Idea::<W>::bind(&mut self.break_away, owner_id);
        Idea::<W>::bind(&mut self.afterburner, owner_id);
        true
    }

    /// The world half of slot 1: face, match throttle (both flags set) and
    /// move get their context.
    fn attach<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.face.set_context(&*cx);
        self.match_throttle.set_context(&*cx);
        self.match_throttle.flag_3a = 1;
        self.match_throttle.flag_3b = 1;
        self.move_idea.set_context(&*cx);
    }

    /// Slot 3 (`BaseFollowerBehavior::Slot3`).
    fn engage<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.consumed = 0;
        self.field_288 = 0;
        self.lane_order = 0;
        self.left_lane = 0;
        cx.set_strafe_dir(StrafeDir::None);
        cx.set_thrusters_active(false);
        cx.set_cruise_lock(1, 3);
        self.dock_running = 0;
        self.goto_running = 0;
        if cx.own().is_some_and(|v| v.is_player) {
            self.player = 1;
        }
        self.delegates = true;
    }

    /// Slot 4 (`BaseFollowerBehavior::Slot4`). A player's ship leaves the
    /// trade lane it was sent into (`CShip::request_exit_tradelane`, which
    /// the AI's world does not offer: a player's follower is P3's).
    fn disengage<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.field_29c = 0;
        self.lane_order = 0;
        self.face.commit(cx);
        self.match_throttle.commit(cx);
        self.move_idea.commit(cx);
        self.break_away.commit(cx);
        Idea::<W>::commit(&mut self.afterburner, cx);
        if self.delegates {
            if self.dock_running != 0 {
                cx.with_peer(super::behavior::ids::DOCK, |b, cx| b.disengage(cx));
            }
            if self.goto_running != 0 {
                cx.with_peer(super::behavior::ids::GOTO, |b, cx| b.disengage(cx));
            }
        }
        self.dock_running = 0;
        self.goto_running = 0;
        self.delegates = false;
        self.base.disengage(cx.state);
    }

    /// The leader validation of slot 7 for a player's ship: the leader
    /// lives, is a ship-like object but no capital ship, runs neither the
    /// follow nor the launch behaviour, and its leader chain does not come
    /// back to the ship. (The group test of two players' ships needs the
    /// ships' group ids, which a player's follower brings with P3.)
    fn player_leader_valid<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>, own_id: u32) -> bool {
        let Some(mut node) = self.lead_view(cx) else {
            return false;
        };
        let bits = node.type_flags;
        if bits & 0x7f_0000 == 0 || bits & 0x20_0000 != 0 {
            return false;
        }
        if BARRED_LEADER_BEHAVIORS.contains(&node.behavior_id) {
            return false;
        }
        // The out-parameter is zeroed once, before the walk.
        let mut leader: Option<u32> = None;
        loop {
            if node.follow_leader.is_some() {
                leader = node.follow_leader;
            } else if node.formation_leader.is_some() {
                leader = node.formation_leader;
            }
            if leader == Some(node.id) {
                return true;
            }
            if leader == Some(own_id) {
                return false;
            }
            let Some(next) = cx.view_of(leader) else {
                return true;
            };
            node = next;
        }
    }

    /// `UpdateTickPayload(Delta)`: the formation exit of the personality
    /// (`FormationExitMode`).
    fn exit_formation<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, dt: f32) {
        let p = cx.state.personality.formation;
        match p.exit_mode {
            0 | 1 => {
                if p.exit_mode == 1 {
                    Idea::<W>::update(&mut self.afterburner, cx, dt);
                }
                let Some(own) = cx.own() else {
                    return;
                };
                let leader_pos = self.lead_view(cx).map_or(Vector::ZERO, |v| v.center);
                self.face.set_direction(sub(own.center, leader_pos));
                self.face.set_scale_94(p.exit_top_turn_break_away_throttle);
                self.face.update(cx);
                let basis = cx.own().map_or(own.orientation, |v| v.orientation);
                self.move_idea.set_direction(neg(column2(&basis)));
                self.move_idea.update(cx, dt);
            }
            2 => {
                cx.set_axis_throttle(Vector::ZERO);
                cx.set_throttle(-1.0, 0);
            }
            3 => {
                self.break_away.roll_throttle = p.exit_roll_outrun_throttle;
                self.break_away.break_style = 0;
                self.break_away.update(cx, dt);
            }
            _ => {}
        }
    }
}

/// The two halves of a follower behaviour the base's slot 7 calls.
trait FollowerWork<W: AiEnv + ?Sized> {
    fn follower(&self) -> &FollowerBase;
    fn follower_mut(&mut self) -> &mut FollowerBase;
    /// Slot 27, the subclass's own work.
    fn work(&mut self, cx: &mut Cx<'_, W>);
    /// Slot 28, the offset handed to the dock delegate.
    fn dock_offset(&self, cx: &Cx<'_, W>) -> Vector;
}

/// `BaseFollowerBehavior::Slot7`, the per-tick body.
fn follower_update<W: AiEnv + ?Sized, B: FollowerWork<W>>(b: &mut B, cx: &mut Cx<'_, W>) -> i32 {
    let result = follower_update_inner(b, cx);
    b.follower_mut().base.result = result;
    result
}

fn follower_update_inner<W: AiEnv + ?Sized, B: FollowerWork<W>>(
    b: &mut B,
    cx: &mut Cx<'_, W>,
) -> i32 {
    let Some(own) = cx.own().filter(|_| b.follower().base.bound) else {
        return 3;
    };
    if b.follower().consumed != 0 {
        return 0;
    }
    let lead = cx.state.sys.formation.current_leader(&*cx.world);
    b.follower_mut().lead = lead;
    if own.is_player && !b.follower().player_leader_valid(cx, own.id) {
        return 3;
    }
    if cx.state.sys.formation.value_b4.1 {
        let f = b.follower_mut();
        f.match_throttle.commit(cx);
        f.face.commit(cx);
        return 1;
    }
    let Some(watched) = b.follower().lead_view(cx) else {
        return 2;
    };
    let dt = b.follower().base.tick_delta;
    if b.follower().player != 0 {
        let close = is_within_intercept_range_of_iface(&own, &watched, 200.0);
        if !close && len_sq_xyz(watched.velocity) < 100.0 {
            if b.follower().goto_running == 0 {
                let op = DirectiveOp::Goto(GotoOp {
                    kind: 0,
                    field_1c: i32::from_ne_bytes(watched.id.to_ne_bytes()),
                    range: 100.0,
                    ..GotoOp::default()
                });
                cx.with_peer(super::behavior::ids::GOTO, |g, cx| {
                    g.accept_directive(cx, &op);
                    g.engage(cx);
                });
                b.follower_mut().goto_running = 1;
            }
            let child = cx
                .with_peer(super::behavior::ids::GOTO, |g, cx| {
                    g.base_mut().tick_delta = dt;
                    g.update(cx)
                })
                .unwrap_or(3);
            if child == 1 || child == 0 {
                return child;
            }
        }
        b.follower_mut().player = 0;
    } else if b.follower().goto_running != 0 {
        b.follower_mut().goto_running = 0;
        cx.with_peer(super::behavior::ids::GOTO, |g, cx| g.disengage(cx));
    }

    if b.follower().dock_running != 0 && b.follower().delegates {
        let child = cx
            .with_peer(super::behavior::ids::DOCK, |d, cx| {
                d.base_mut().tick_delta = dt;
                d.update(cx)
            })
            .unwrap_or(3);
        if child == 1 || child == 0 {
            return child;
        }
        b.follower_mut().dock_running = 0;
        cx.with_peer(super::behavior::ids::DOCK, |d, cx| d.disengage(cx));
    }

    let order = cx.state.sys.formation.order;
    let run_pending_lookup = if b.follower().lane_order != 0 {
        if order.pending || b.follower().left_lane == 0 {
            return 0;
        }
        b.follower_mut().left_lane = 0;
        b.follower_mut().lane_order = 0;
        true
    } else {
        !order.pending
    };

    if !run_pending_lookup {
        // Not null-tested in the reference: the order's object is there.
        let candidate = cx.view_of(Some(order.object));
        let (radius, center) = candidate
            .as_ref()
            .map_or((0.0, Vector::ZERO), |c| (c.radius, c.center));
        let mut commit = order.commit_directly;
        if !commit {
            let dx = e(own.center.x) - e(center.x);
            let dy = e(own.center.y) - e(center.y);
            let dz = e(own.center.z) - e(center.z);
            let distance = ((dz * dz + dx * dx) + dy * dy).sqrt();
            commit = distance < e(radius);
        }
        if commit {
            let entrance = cx.world.handle_of(order.object);
            let exit = cx.world.handle_of(order.second_object);
            let committed = match (entrance, exit) {
                (Some(entrance), Some(exit)) => cx
                    .world
                    .command(cx.ship, AiCommand::GoTradeLane { entrance, exit }),
                _ => -2,
            };
            if committed != 0 {
                return 1;
            }
            b.follower_mut().lane_order = 1;
            return 0;
        }
        let _offset = b.dock_offset(cx);
        cx.with_peer(super::behavior::ids::DOCK, |d, cx| {
            d.accept_directive(cx, &DirectiveOp::Other(13));
            d.engage(cx);
        });
        b.follower_mut().dock_running = 1;
        return 1;
    }

    if cx.state.sys.formation.value_b0.is_some() {
        cx.state.sys.formation.value_b0 = None;
        b.follower_mut().consumed = 1;
        return 0;
    }

    let personality = cx.state.personality;
    let behavior = cx.state.behavior;
    let formation = &cx.state.sys.formation;
    let gate_a = formation.gate_a(&*cx.world, own.is_player, behavior, &personality);
    let mut payload = false;
    if (!gate_a || formation.gate_b(own.is_player, &personality))
        && formation.gate_b(own.is_player, &personality)
        && personality.job.force_attack_formation != 0
    {
        payload = true;
    }
    if payload {
        b.follower_mut().exit_formation(cx, dt);
    } else {
        b.work(cx);
    }
    1
}

/// `VecCosineProject3(A, B)`: `A`'s length times the clamped cosine of
/// the angle between the two, along `B`'s direction.
fn vec_cosine_project3(a: Vector, b: Vector) -> Vector {
    let norm_a = super::fp::normalize3(a);
    let len_b = ((e(b.y) * e(b.y) + e(b.z) * e(b.z)) + e(b.x) * e(b.x)).sqrt();
    let (mut dir_x, mut dir_y, mut dir_z) = (0.0_f64, 0.0_f64, 0.0_f32);
    if len_b > e(0.01) {
        let inv = r(1.0 / len_b);
        dir_x = e(inv) * e(b.x);
        dir_y = e(b.y) * e(inv);
        dir_z = r(e(b.z) * e(inv));
    }
    // NaN stays NaN, as in the reference.
    let dot =
        ((e(norm_a.y) * dir_y + e(dir_z) * e(norm_a.z)) + dir_x * e(norm_a.x)).clamp(-1.0, 1.0);
    let mag = r(len_sq_xyz(a).sqrt());
    let scale = r(e(mag) * dot);
    Vector::new(
        r(dir_x * e(scale)),
        r(dir_y * e(scale)),
        r(e(dir_z) * e(scale)),
    )
}

/// The `0.0 < c*c` test of the follow offset: present unless every square
/// compares at most 0 (NaN counts as present).
fn has_offset(v: Vector) -> bool {
    let sq = |c: f32| e(c) * e(c);
    !(sq(v.x) <= 0.0) || !(sq(v.y) <= 0.0) || !(sq(v.z) <= 0.0)
}

/// `FollowBehavior` (behaviour 17, 0x2bc bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FollowBehavior {
    /// The base follower block.
    pub f: FollowerBase,
    /// +0x2a0 the follow distance (150; the directive's).
    pub speed_param: f32,
    /// +0x2a4 the leader is within the follow range (1).
    pub enabled: u8,
    /// +0x2a8 the offset's length.
    pub distance: f32,
    /// +0x2ac (150).
    pub field_2ac: f32,
    /// +0x2b0 the weight of the leader's speed (2).
    pub field_2b0: f32,
    /// +0x2b4 the poll's range (400).
    pub field_2b4: f32,
    /// +0x2b8 the range bias of the engage test (100).
    pub field_2b8: f32,
}

impl FollowBehavior {
    /// `FollowBehavior::FollowBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            f: FollowerBase::new(id),
            speed_param: 150.0,
            enabled: 1,
            distance: 0.0,
            field_2ac: 150.0,
            field_2b0: 2.0,
            field_2b4: 400.0,
            field_2b8: 100.0,
        }
    }

    /// The engage toggle slot 27 opens with: `enabled` follows the
    /// leader's crossing of the follow range. Each transition goes to
    /// `IBehaviorManager::NotifyFollowEvent`, which reads and changes
    /// nothing without a content callback, and only reports to one.
    fn update_engage_state(&mut self, own: &ObjectView, leader: &ObjectView) {
        let range = r(e(self.field_2b8) + e(self.speed_param));
        if is_within_intercept_range_of_iface(own, leader, range) {
            if self.enabled == 0 {
                self.enabled = 1;
            }
        } else if self.enabled != 0 {
            self.enabled = 0;
        }
    }
}

impl<W: AiEnv + ?Sized> FollowerWork<W> for FollowBehavior {
    fn follower(&self) -> &FollowerBase {
        &self.f
    }

    fn follower_mut(&mut self) -> &mut FollowerBase {
        &mut self.f
    }

    /// Slot 27: the follow point is the offset in the leader's frame (or
    /// the leader's `generate_follow_offset` without one); the match
    /// throttle matches the leader there, the face idea looks at the point
    /// from afar and at a point ahead of it, scaled by the leader's speed,
    /// once close.
    fn work(&mut self, cx: &mut Cx<'_, W>) {
        let Some(leader) = self.f.lead_view(cx) else {
            return;
        };
        let Some(own) = cx.own() else {
            return;
        };
        let mut basis = leader.orientation;
        let mut speed = self.speed_param;
        let offset = self.f.offset;
        let present = has_offset(offset);
        let generated = if present {
            None
        } else {
            match (cx.world.handle_of(leader.id), Some(cx.ship)) {
                (Some(lh), Some(ship)) => cx.world.generate_follow_offset(lh, ship),
                _ => None,
            }
        };
        let (target_point, watch_block) = if let Some(probe) = generated {
            basis = probe.rotation;
            speed = 50.0;
            (probe.translation, false)
        } else {
            let rotated = transform_matrix(&basis, offset);
            (add(rotated, leader.center), !present)
        };
        let mut update_match = true;
        self.update_engage_state(&own, &leader);
        if watch_block {
            self.f
                .match_throttle
                .set_watch_and_distance(cx, Some(leader.id), speed);
            self.f.face.set_watch_object(Some(leader.id));
        } else {
            self.f
                .match_throttle
                .set_watch_and_position(cx, Some(leader.id), target_point);
            let clearance = get_intercept_clearance_of_iface(&own, &leader);
            if !(clearance < e(self.distance) + 100.0) {
                self.f.face.set_position(target_point);
            } else {
                let leader_speed = iface_vector_magnitude(Some(&leader));
                let mut weight = r(leader_speed);
                if leader_speed < 1.0 {
                    weight = 1.0;
                    update_match = false;
                    self.f.match_throttle.flag_39 = 0;
                    self.f.match_throttle.commit(cx);
                    self.f.match_throttle.flag_39 = self.f.flag_28c;
                }
                let forward = column2(&basis);
                let own_now = cx.own().unwrap_or(own.clone());
                let delta = sub(own_now.center, target_point);
                let projected = vec_cosine_project3(delta, forward);
                let mut length = r(len_sq_xyz(projected).sqrt());
                if length < 0.0 {
                    length = 0.0;
                }
                let scalar = (e(weight) * e(self.field_2b0) + e(own_now.radius)) + e(length);
                let product_x = r(-e(forward.x) * scalar);
                let product_y = r(-e(forward.y) * scalar);
                let product_z = -e(forward.z) * scalar;
                let aim = Vector::new(
                    r(e(product_x) + e(target_point.x)),
                    r(e(product_y) + e(target_point.y)),
                    r(product_z + e(target_point.z)),
                );
                self.f.face.set_position(aim);
            }
        }
        if update_match {
            let timer4 = cx.timer_active(slot::AFTERBURNER);
            self.f.match_throttle.update(cx, timer4);
        }
        // `GetInterceptClearanceOfIface` again, for effect.
        self.f.face.update(cx);
    }

    /// Slot 28: the offset in the leader's frame.
    fn dock_offset(&self, cx: &Cx<'_, W>) -> Vector {
        let basis = self
            .f
            .lead_view(cx)
            .map_or(Matrix::IDENTITY, |v| v.orientation);
        transform_matrix(&basis, self.f.offset)
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for FollowBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.f.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.f.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        self.f.bind::<W>(owner_id)
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        self.f.attach(cx);
    }

    /// Slot 3: the base's, then the leader learns its new follower and the
    /// formation block its follow leader.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.f.engage(cx);
        let Some(lead) = self.f.lead_view(cx) else {
            return;
        };
        if let Some(lh) = cx.world.handle_of(lead.id) {
            cx.world.command(lh, AiCommand::AddFollowFollower(cx.ship));
        }
        let own_id = self.f.base.owner_id;
        let lead = self.f.lead_view(cx).map(|v| v.id);
        cx.state
            .sys
            .formation
            .set_follow_leader(&mut *cx.world, cx.ship, own_id, lead);
    }

    /// Slot 4: the base's, then the leader drops the follower and the
    /// formation block its follow leader.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.f.disengage(cx);
        let Some(lead) = self.f.lead_view(cx) else {
            return;
        };
        if let Some(lh) = cx.world.handle_of(lead.id) {
            cx.world
                .command(lh, AiCommand::RemoveFollowFollower(cx.ship));
        }
        let own_id = self.f.base.owner_id;
        cx.state
            .sys
            .formation
            .set_follow_leader(&mut *cx.world, cx.ship, own_id, None);
    }

    /// Slot 5: without a follow directive or leader it declines (2); with
    /// them 1 within the directive's range of the leader, else 0.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let lead = self.f.lead_view(cx);
        let mode = match (self.f.base.flags & 0x2 != 0, lead, cx.own()) {
            (true, Some(lead), Some(own)) => {
                if is_within_intercept_range_of_iface(&own, &lead, self.field_2b4) {
                    1
                } else {
                    0
                }
            }
            _ => 2,
        };
        self.f.base.mode = if self.f.base.flag29 != 0 { 3 } else { mode };
        self.f.base.mode
    }

    /// Slot 6: blocked when the target is a ship-like object and the ship
    /// is not in a trade lane.
    fn evaluate(&mut self, cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        let Some(target) = cx.target_view() else {
            return EvaluateResult::Skip;
        };
        if cx.own().and_then(|v| v.cruise).is_some_and(|c| c.1) {
            return EvaluateResult::Skip;
        }
        if object_type_or_zero(Some(&target)) & 0x7f_0000 != 0 {
            return EvaluateResult::Blocked;
        }
        EvaluateResult::Skip
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        follower_update(self, cx)
    }

    /// Slot 8: the match-throttle idea's cruise follow, unless the dock
    /// delegate runs or a lane order is out.
    fn end_tick(&mut self, cx: &mut Cx<'_, W>) {
        if self.f.dock_running != 0 || self.f.lane_order != 0 {
            return;
        }
        self.f.match_throttle.slot2(cx);
    }

    /// Slot 10: the follow directive's leader, offset and distance; the
    /// ship's follow offset and the formation block's follow leader. Without
    /// the leader the op's range is kept and the directive refused.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        if let DirectiveOp::Follow(op) = op {
            self.f.lead = cx
                .view_of((op.target != 0).then_some(op.target))
                .map(|v| v.id);
            if self.f.lead.is_some() {
                let o = op.offset;
                self.f.offset = o;
                self.distance = r(((e(o.x) * e(o.x) + e(o.y) * e(o.y)) + e(o.z) * e(o.z)).sqrt());
                self.speed_param = op.distance;
                cx.world.command(cx.ship, AiCommand::SetFollowOffset(o));
                let own_id = self.f.base.owner_id;
                let lead = self.f.lead;
                cx.state
                    .sys
                    .formation
                    .set_follow_leader(&mut *cx.world, cx.ship, own_id, lead);
                self.f.base.flags |= 0x2;
                return true;
            }
            self.field_2b4 = op.range;
        }
        self.f.base.flags &= 0xfd;
        false
    }

    /// Slot 16: nothing.
    fn set_afterburner(&mut self, _cx: &mut Cx<'_, W>, _on: bool) {}

    /// Slot 17: the strafe is released whatever the direction.
    fn slide_strafe_burst(&mut self, cx: &mut Cx<'_, W>, _dir: StrafeDir) {
        cx.set_strafe_dir(StrafeDir::None);
    }

    fn allow_head_tracking(&self) -> bool {
        false
    }

    /// Slot 21: the leader left.
    fn object_left(&mut self, _cx: &mut Cx<'_, W>, id: u32) {
        if self.f.lead == Some(id) {
            self.f.lead = None;
        }
    }
}

/// `FormationBehavior` (behaviour 10, 0x2b8 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FormationBehavior {
    /// The base follower block.
    pub f: FollowerBase,
    /// +0x2a0 the stand-off of the face point (200).
    pub standoff: f32,
    /// +0x2a4 the stand-off while cruising outside a lane (500).
    pub cruise_standoff: f32,
    /// +0x2a8 the player steers.
    pub direction_set: u8,
    /// +0x2ac the player's steering axis.
    pub face_axis: Vector,
}

impl FormationBehavior {
    /// `FormationBehavior::FormationBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            f: FollowerBase::new(id),
            standoff: 200.0,
            cruise_standoff: 500.0,
            direction_set: 0,
            face_axis: Vector::ZERO,
        }
    }

    /// The leader slot 5 and slot 27 point the watcher at: none while the
    /// ship has a follow leader (slot 5 only), else the formation leader.
    fn formation_leader<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> Option<u32> {
        cx.state.sys.formation.formation_leader(&*cx.world)
    }
}

impl<W: AiEnv + ?Sized> FollowerWork<W> for FormationBehavior {
    fn follower(&self) -> &FollowerBase {
        &self.f
    }

    fn follower_mut(&mut self) -> &mut FollowerBase {
        &mut self.f
    }

    /// Slot 27: the slot point is the own offset in the leader's frame
    /// from the formation's anchor (the leader's position less its own
    /// offset); the match throttle holds it, the face idea looks at a
    /// point ahead of it (by the stand-off and the ship's radius while the
    /// ship heads the leader's way).
    fn work(&mut self, cx: &mut Cx<'_, W>) {
        self.f.lead = Self::formation_leader(cx);
        let Some(leader) = self.f.lead_view(cx) else {
            return;
        };
        let Some(own) = cx.own() else {
            return;
        };
        let basis = leader.orientation;
        if let Some(o) = own.formation_offset {
            self.f.offset = o;
        }
        let leader_offset = leader.formation_offset.unwrap_or(Vector::ZERO);
        let anchor = sub(leader.center, transform_matrix(&basis, leader_offset));
        let slot_point = add(transform_matrix(&basis, self.f.offset), anchor);
        let leader_backward = negated_iface_direction(Some(&leader));
        let (cruise, lane) = own.cruise.unwrap_or((false, false));
        let standoff = if cruise && !lane {
            self.cruise_standoff
        } else {
            self.standoff
        };
        let own_backward = neg(column2(&own.orientation));
        let facing = super::fp::dot_zyx(own_backward, leader_backward);
        let mut clearance = 0.0_f64;
        if facing > 0.0 {
            let reach = e(own.radius) + e(standoff);
            if !(reach < 0.0) {
                clearance = reach;
            }
        }
        let offset = self.f.offset;
        let adjusted = Vector::new(offset.x, offset.y, r(e(offset.z) - clearance));
        let target_point = add(transform_matrix(&basis, adjusted), anchor);
        self.f.face.set_position(target_point);
        if cx.timer_active(slot::ROLL) {
            self.f.face.set_up_vector(Vector::new(0.0, 0.0, 1.0));
        }
        if self.direction_set != 0 {
            self.f.face.set_axis(self.face_axis);
            self.f
                .move_idea
                .set_direction(neg(column2(&own.orientation)));
        }
        self.f.face.update(cx);
        self.f
            .match_throttle
            .set_watch_and_position(cx, Some(leader.id), slot_point);
        let timer4 = cx.timer_active(slot::AFTERBURNER);
        self.f.match_throttle.update(cx, timer4);
    }

    /// Slot 28: the ship's formation offset in the leader's frame.
    fn dock_offset(&self, cx: &Cx<'_, W>) -> Vector {
        let offset = cx
            .own()
            .and_then(|v| v.formation_offset)
            .unwrap_or(Vector::ZERO);
        let basis = self
            .f
            .lead_view(cx)
            .map_or(Matrix::IDENTITY, |v| v.orientation);
        transform_matrix(&basis, offset)
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for FormationBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.f.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.f.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        self.f.bind::<W>(owner_id)
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        self.f.attach(cx);
    }

    /// Slot 3: the base's, the ship's formation offset; a player's ship
    /// latches it and sets its ideas for the player.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.f.engage(cx);
        let own = cx.own();
        if let Some(o) = own.as_ref().and_then(|v| v.formation_offset) {
            self.f.offset = o;
        }
        if own.is_some_and(|v| v.is_player) {
            self.f.flag_28c = 1;
            self.f.match_throttle.flag_39 = 1;
            self.f.move_idea.set_field_44(2);
        }
        self.direction_set = 0;
    }

    /// Slot 4: the base's; a player's ship asks to be released.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.f.disengage(cx);
        if !cx.own().is_some_and(|v| v.is_player) {
            return;
        }
        cx.state
            .sys
            .formation
            .request_release(&mut *cx.world, cx.ship);
        let id = cx.own().map_or(0, |v| v.id);
        cx.world.command(
            cx.ship,
            AiCommand::Slot {
                slot: super::gun_convergence::REQUEST_RELEASE_SLOT,
                ints: vec![1, i64::from(id)],
                floats: Vec::new(),
            },
        );
    }

    /// Slot 5: 1 with a formation leader (and no follow leader) or once
    /// the parked order was consumed, else 2.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = if self.f.consumed != 0 { 1 } else { 2 };
        self.f.lead = if cx.state.sys.formation.follow_leader(&*cx.world).is_some() {
            None
        } else {
            Self::formation_leader(cx)
        };
        if self.f.lead_view(cx).is_some() {
            result = 1;
        }
        self.f.base.mode = if self.f.base.flag29 != 0 { 3 } else { result };
        self.f.base.mode
    }

    /// Slot 6: blocked when the target is a ship-like object within 5000 m
    /// that does not lead back to the ship and feels better than -0.6
    /// toward it. (The group tests of two players' ships need the ships'
    /// group ids, which come with P3.)
    fn evaluate(&mut self, cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        let (Some(own), Some(target)) = (cx.own(), cx.target_view()) else {
            return EvaluateResult::Skip;
        };
        if own.cruise.is_some_and(|c| c.1) {
            return EvaluateResult::Skip;
        }
        let bits = target.type_flags;
        if bits & 0x7f_0000 == 0 || bits & 0x20_0000 != 0 {
            return EvaluateResult::Skip;
        }
        if BARRED_LEADER_BEHAVIORS.contains(&target.behavior_id) {
            return EvaluateResult::Skip;
        }
        let mut node = target.clone();
        let mut leader: Option<u32> = None;
        loop {
            if node.follow_leader.is_some() {
                leader = node.follow_leader;
            } else if node.formation_leader.is_some() {
                leader = node.formation_leader;
            }
            if leader == Some(node.id) {
                break;
            }
            if leader == Some(own.id) {
                return EvaluateResult::Skip;
            }
            match cx.view_of(leader) {
                Some(next) => node = next,
                None => break,
            }
        }
        let dx = e(target.position.x) - e(own.center.x);
        let dy = e(target.position.y) - e(own.center.y);
        let dz = e(target.position.z) - e(own.center.z);
        if !((dx * dx + dz * dz) + dy * dy <= 25_000_000.0) {
            return EvaluateResult::Skip;
        }
        let attitude = match (cx.world.handle_of(target.id), Some(cx.ship)) {
            (Some(th), Some(ship)) => cx.world.attitude(th, ship).unwrap_or(0.0),
            _ => 0.0,
        };
        if attitude > -0.6 {
            return EvaluateResult::Blocked;
        }
        EvaluateResult::Skip
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        follower_update(self, cx)
    }

    /// Slot 8: as the follow behaviour's.
    fn end_tick(&mut self, cx: &mut Cx<'_, W>) {
        if self.f.dock_running != 0 || self.f.lane_order != 0 {
            return;
        }
        self.f.match_throttle.slot2(cx);
    }

    /// Slot 10: takes any directive.
    fn accept_directive(&mut self, _cx: &mut Cx<'_, W>, _op: &DirectiveOp) -> bool {
        self.f.base.flags |= 0x2;
        true
    }

    /// Slot 12: the player's steering, scaled by the turn sensitivity and
    /// clamped.
    fn set_direction(&mut self, cx: &mut Cx<'_, W>, dir: Vector) {
        let len_sq = len_sq_xyz(dir);
        if !(len_sq > 0.0) {
            self.direction_set = 0;
            return;
        }
        self.direction_set = 1;
        let mut x = -dir.y;
        let mut y = -dir.x;
        let factor = cx.state.turn_sensitivity;
        if e(factor) > 0.0 {
            let inverse = 1.0 / e(factor);
            x = r(inverse * e(x));
            y = r(inverse * e(y));
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
        self.face_axis = Vector::new(clamp(x), clamp(y), -dir.z);
    }

    fn set_afterburner(&mut self, _cx: &mut Cx<'_, W>, _on: bool) {}

    fn slide_strafe_burst(&mut self, cx: &mut Cx<'_, W>, _dir: StrafeDir) {
        cx.set_strafe_dir(StrafeDir::None);
    }

    fn allow_head_tracking(&self) -> bool {
        false
    }

    fn object_left(&mut self, _cx: &mut Cx<'_, W>, id: u32) {
        if self.f.lead == Some(id) {
            self.f.lead = None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cosine_projection_along_b() {
        let p = vec_cosine_project3(Vector::new(3.0, 0.0, 4.0), Vector::new(0.0, 0.0, 2.0));
        assert_eq!(p, Vector::new(0.0, 0.0, 4.0));
        assert_eq!(
            vec_cosine_project3(Vector::new(1.0, 0.0, 0.0), Vector::ZERO),
            Vector::ZERO
        );
    }

    #[test]
    fn offset_presence() {
        assert!(!has_offset(Vector::ZERO));
        assert!(has_offset(Vector::new(0.0, 0.0, 1.0)));
        assert!(has_offset(Vector::new(f32::NAN, 0.0, 0.0)));
    }

    #[test]
    fn defaults() {
        let f = FollowBehavior::new(17);
        assert_eq!((f.speed_param, f.enabled, f.field_2b4), (150.0, 1, 400.0));
        let g = FormationBehavior::new(10);
        assert_eq!((g.standoff, g.cruise_standoff), (200.0, 500.0));
    }
}
