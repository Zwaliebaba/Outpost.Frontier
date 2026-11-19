//! `FleeBehavior` (behaviour 4, `Common/common_fleebehavior.cpp`, class in
//! `include/common_fleebehavior.h`): the poll decides to flee (the leader
//! flees, the hull and shield are low, the hostile power is high, or no
//! gun is ready); the flee itself asks the content callback for a
//! destination and hands the ship to the manager's goto behaviour (a point
//! or an object), dock behaviour or, when the leader flees, formation
//! behaviour, which it ticks in its own slots.
//!
//! The port has no content callback to ask (`request_flee_destination` is
//! Content's, P3.6), so the destination is the reference's fallback: the
//! ship's centre 10 km along x, within 500 m (D184). The dock and formation
//! behaviours are not ported: while the flee drives one of them it reports
//! itself not ported.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, GotoOp, ids};
use super::fp::{e, r};
use super::goto::GotoBehavior;

/// The fallback destination's offset along x (`0x461c4000`).
const FLEE_DESTINATION_OFFSET_X: f32 = 10_000.0;
/// The fallback destination's radius (`0x43fa0000`).
const FLEE_RADIUS: f32 = 500.0;

/// The flee hostile-ratio thresholds of `FleeSceneThreatStyle`.
fn flee_hostile_ratio_threshold(level: i32) -> f32 {
    match level {
        0 => 0.15,
        1 => 0.35,
        2 => 0.65,
        3 => 0.85,
        4 => 1.0,
        _ => 0.5,
    }
}

/// `FleeBehavior` (0xe8 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FleeBehavior {
    base: BehaviorBase,
    /// +0x2c a flee is running.
    pub field_2c: u8,
    /// +0x30 the flee reason (`FleeReason`: 0 the hostile power, 1 the
    /// leader flees, 2 the damage, 3 no weapons; 4 none yet).
    pub field_30: i32,
    /// +0x34 the destination type (1 an object, 2 a dock, 3 a point).
    pub field_34: i32,
    /// +0x38 the leader flees (the formation behaviour drives).
    pub field_38: u8,
    /// +0x48 the goto (or dock) directive running before the flee was
    /// saved, to be handed back when the flee ends.
    pub field_48: u8,
    /// +0x4c the saved goto directive.
    pub goto_op: GotoOp,
}

impl FleeBehavior {
    /// `FleeBehavior::FleeBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            field_2c: 0,
            field_30: 4,
            field_34: 0,
            field_38: 0,
            field_48: 0,
            goto_op: GotoOp::default(),
        }
    }

    fn answer(&mut self, mode: i32) -> i32 {
        self.base.mode = if self.base.flag29 != 0 { 3 } else { mode };
        self.base.mode
    }

    /// The behaviour the flee drives: formation while the leader flees,
    /// else dock for type 2 and goto for types 1 and 3; `None` for an
    /// uninitialised type.
    fn delegate(&self) -> Option<i32> {
        if self.field_38 != 0 {
            return Some(ids::FORMATION);
        }
        match self.field_34 {
            1 | 3 => Some(ids::GOTO),
            2 => Some(ids::DOCK),
            _ => None,
        }
    }

    /// The first tick of a flee: the destination (the fallback, D184) and
    /// the delegate's directive and slot 3.
    fn start<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.field_2c = 1;
        // `RequestFleeDestination`: no callback answers (D184). The
        // fallback: type 3, the cached physical centre 10 km along x.
        let kind = 3;
        let center = cx.own().map_or(Vector::ZERO, |v| v.center);
        let destination = Vector::new(
            r(e(center.x) + e(FLEE_DESTINATION_OFFSET_X)),
            center.y,
            center.z,
        );
        let destination_id = 0u32;
        let radius = FLEE_RADIUS;
        self.field_34 = kind;
        match self.field_34 {
            2 => {
                // A `DirectiveDockOp` for the destination: the dock
                // behaviour is not ported (its op has no fields here).
                self.field_48 = 0;
                cx.with_peer(ids::DOCK, |b, cx| {
                    b.accept_directive(cx, &DirectiveOp::Other(13));
                    b.engage(cx);
                });
            }
            1 | 3 => {
                let mut op = GotoOp::default();
                if self.field_34 == 3 {
                    op.kind = 1;
                    op.destination = destination;
                } else {
                    op.kind = 0;
                    op.field_1c = i32::from_ne_bytes(destination_id.to_ne_bytes());
                }
                op.range = radius;
                self.field_48 = 0;
                if cx.state.directive[2] == ids::GOTO {
                    // `CopyActiveGotoOperation`: the goto's directive and
                    // its directive flag.
                    let saved = cx.with_peer(ids::GOTO, |b, _| {
                        let active = b.base().flags & 0x2 != 0;
                        b.as_any()
                            .and_then(|a| a.downcast_ref::<GotoBehavior>())
                            .map(|g| (g.op.clone(), active))
                    });
                    if let Some(Some((copy, active))) = saved {
                        self.goto_op = copy;
                        if active {
                            self.field_48 = 1;
                        }
                    }
                }
                let op = DirectiveOp::Goto(op);
                cx.with_peer(ids::GOTO, |b, cx| {
                    b.accept_directive(cx, &op);
                    b.engage(cx);
                });
            }
            // The reference reports the uninitialised type (`FDUMP`).
            _ => {}
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for FleeBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    /// Not ported while the flee drives the formation or dock behaviour.
    fn ported(&self) -> bool {
        !matches!(self.delegate(), Some(ids::FORMATION | ids::DOCK)) || self.field_2c == 0
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    /// Slot 3: with a flee running, the delegate's slot 3 (the reference
    /// formats a diagnostic it never reads for an uninitialised type).
    /// `Behavior::Slot3` does not run.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        if self.field_2c == 0 {
            return;
        }
        if let Some(index) = self.delegate() {
            cx.with_peer(index, |b, cx| b.engage(cx));
        }
    }

    /// Slot 4: the delegate's slot 4; a goto (or dock) directive saved at
    /// the start goes back to its behaviour when it is still the current
    /// directive. `Behavior::Slot4` does not run, and the flee stays
    /// running.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        if self.field_38 != 0 {
            cx.with_peer(ids::FORMATION, |b, cx| b.disengage(cx));
            return;
        }
        match self.field_34 {
            1 | 3 => {
                let restore = cx.state.directive[2] == ids::GOTO && self.field_48 != 0;
                let op = DirectiveOp::Goto(self.goto_op.clone());
                cx.with_peer(ids::GOTO, |b, cx| {
                    b.disengage(cx);
                    if restore {
                        b.accept_directive(cx, &op);
                    }
                });
            }
            2 => {
                let restore = cx.state.directive[2] == ids::DOCK && self.field_48 != 0;
                cx.with_peer(ids::DOCK, |b, cx| {
                    b.disengage(cx);
                    // `Behavior_ReleaseDirectiveTarget`.
                    let base = b.base_mut();
                    base.flags = (base.flags & 0xfd) | 0x01;
                    if restore {
                        b.accept_directive(cx, &DirectiveOp::Other(13));
                    }
                });
            }
            _ => {}
        }
    }

    /// Slot 5: 0 when the leader flees, the hull and shield are below the
    /// flee percentage, the hostile power ratio is above the threat
    /// style's threshold, or (with the no-weapons style) no gun is ready;
    /// a running flee answers 0.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let job = cx.state.personality.job;
        self.field_38 = 0;
        let mut result = 2;
        if job.flee_when_leader_flees_style != 0
            && let Some(leader) = cx.state.sys.formation.current_leader(&*cx.world)
            && cx
                .world
                .handle_of(leader)
                .and_then(|h| cx.world.manager_of(h))
                .is_some_and(|m| m.0 == ids::FLEE)
        {
            self.field_38 = 1;
            self.field_2c = 1;
            self.field_30 = 1;
        }
        if self.field_2c != 0 {
            return self.answer(0);
        }
        let own = cx.own();
        let (shield, shield_max) = own
            .as_ref()
            .and_then(|v| v.shield)
            .map_or((0.0, 0.0), |s| (s.0, s.1));
        let (hp, hp_max) = own.as_ref().map_or((0.0, 0.0), |v| v.hit_points);
        let mut flee = false;
        let ratio = (e(hp) + e(shield)) / (e(hp_max) + e(shield_max));
        if ratio < e(job.flee_when_hull_damaged_percent) {
            flee = true;
            self.field_30 = 2;
        }
        let aggression = cx.state.sys.targeting.aggression_ratio;
        if e(aggression) > e(flee_hostile_ratio_threshold(job.flee_scene_threat_style)) {
            flee = true;
            self.field_30 = 0;
        }
        if job.flee_no_weapons_style != 0
            && !cx.state.sys.guns.has_ready_weapon_slot(&*cx.world, cx.ship)
        {
            self.field_30 = 3;
            result = 0;
        } else if flee {
            result = 0;
        }
        self.answer(result)
    }

    /// Slot 7 (`Perform`): the first tick starts the flee; then the
    /// formation behaviour's slots 3 and 7 while the leader flees, or the
    /// delegate's slot 7 with this tick's time until it answers 2 or 3,
    /// which ends the flee (`SetDirectiveResult(4, 0)`, 2).
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if self.field_2c == 0 {
            self.start(cx);
            if self.field_2c == 0 {
                self.base.result = 1;
                return 1;
            }
        }
        let dt = self.base.tick_delta;
        if self.field_38 != 0 {
            cx.with_peer(ids::FORMATION, |b, cx| {
                b.engage(cx);
                b.base_mut().tick_delta = dt;
                b.update(cx);
            });
            self.base.result = 1;
            return 1;
        }
        if let Some(index @ (ids::GOTO | ids::DOCK)) = self.delegate() {
            let child = cx
                .with_peer(index, |b, cx| {
                    b.base_mut().tick_delta = dt;
                    b.update(cx)
                })
                .unwrap_or(3);
            if child != 2 && child != 3 {
                self.base.result = 1;
                return 1;
            }
        }
        self.field_2c = 0;
        cx.set_directive_result(ids::FLEE, 0);
        self.base.result = 2;
        2
    }
}
