//! The behaviours and their factory (`CREATE_BEHAVIOR`,
//! `common_behaviortypes.cpp`). Ported so far: the simple behaviours of
//! `common_nullbehavior.cpp`, `common_idlebehavior.cpp`,
//! `common_delaybehavior.cpp`, `common_cruisebehavior.cpp` and
//! `common_drasticevadebehavior.cpp`. Every other id makes an
//! [`UnportedBehavior`], which keeps the manager's protocol (it binds,
//! engages and ticks) but never volunteers in a re-evaluation and commands
//! nothing; [`Behavior::ported`] tells a replay to skip its ticks.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, ids};
use super::fp::{e, r};
use super::idea::{Idea, IdleIdea};
use super::world::AiCommand;

/// `CREATE_BEHAVIOR(id)`: the behaviour registered under `id`.
#[must_use]
pub fn create_behavior<W: AiEnv + ?Sized>(id: i32) -> Box<dyn Behavior<W>> {
    match id {
        ids::NULL => Box::new(NullBehavior::new(id)),
        ids::IDLE => Box::new(IdleBehavior::new(id)),
        ids::CRUISE => Box::new(CruiseBehavior::new(id)),
        ids::DRASTIC_EVADE => Box::new(DrasticEvadeBehavior::new(id)),
        ids::DELAY => Box::new(DelayBehavior::new(id)),
        _ => Box::new(UnportedBehavior::new(id)),
    }
}

/// The dword at `offset` of a directive op as the reference lays it out
/// (`BaseOp` is `vptr`, `OpType`, `Priority`; the fields start at 0x0c);
/// 0 where the port keeps no field. Slot-10 bodies read their op by
/// offset whatever its type.
#[must_use]
pub fn op_dword(op: &DirectiveOp, offset: usize) -> u32 {
    let bits = |v: f32| v.to_bits();
    let int = |v: i32| u32::from_ne_bytes(v.to_ne_bytes());
    match (op, offset) {
        (DirectiveOp::Goto(g), 0x0c) => int(g.kind),
        (DirectiveOp::Goto(g), 0x10) => bits(g.destination.x),
        (DirectiveOp::Goto(g), 0x14) => bits(g.destination.y),
        (DirectiveOp::Goto(g), 0x18) => bits(g.destination.z),
        (DirectiveOp::Goto(g), 0x1c) => int(g.field_1c),
        (DirectiveOp::Face(f), 0x0c) => int(f.mode),
        (DirectiveOp::Face(f), 0x10) => f.target,
        (DirectiveOp::Face(f), 0x14) => bits(f.point.x),
        (DirectiveOp::Face(f), 0x18) => bits(f.point.y),
        (DirectiveOp::Face(f), 0x1c) => bits(f.point.z),
        (DirectiveOp::DrasticEvade(d), 0x0c) => d.threat,
        (DirectiveOp::DrasticEvade(d), 0x10) => d.words_10[0],
        (DirectiveOp::DrasticEvade(d), 0x14) => d.words_10[1],
        (DirectiveOp::DrasticEvade(d), 0x18) => d.words_10[2],
        (DirectiveOp::DrasticEvade(d), 0x1c) => bits(d.duration),
        (DirectiveOp::Delay(s), 0x0c) => bits(*s),
        _ => 0,
    }
}

/// A behaviour the port does not have yet.
#[derive(Clone, Debug, PartialEq)]
pub struct UnportedBehavior {
    base: BehaviorBase,
}

impl UnportedBehavior {
    /// Behaviour `id`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for UnportedBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn ported(&self) -> bool {
        false
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        2
    }

    fn update(&mut self, _cx: &mut Cx<'_, W>) -> i32 {
        self.base.result = 1;
        1
    }
}

/// `NullBehavior` (id 0): binds without the binder, answers 1 to the
/// poll and 2 from its tick, and keeps neither.
#[derive(Clone, Debug, PartialEq)]
pub struct NullBehavior {
    base: BehaviorBase,
}

impl NullBehavior {
    /// `NullBehavior::NullBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for NullBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn bind(&mut self, _owner_id: Option<u32>) -> bool {
        true
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        1
    }

    fn update(&mut self, _cx: &mut Cx<'_, W>) -> i32 {
        2
    }
}

/// `IdleBehavior` (id 6): stops the ship through its idle idea.
#[derive(Clone, Debug, PartialEq)]
pub struct IdleBehavior {
    base: BehaviorBase,
    /// +0x2c the idle idea.
    pub idea: IdleIdea,
}

impl IdleBehavior {
    /// `IdleBehavior::IdleBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            idea: IdleIdea::default(),
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for IdleBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let ok = self.base.bind(owner_id);
        if ok {
            Idea::<W>::bind(&mut self.idea, owner_id);
        }
        ok
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.idea.commit(cx);
        self.base.disengage(cx.state);
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        self.base.mode_from_flags()
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if !self.base.bound {
            self.base.result = 3;
            return 3;
        }
        let dt = self.base.tick_delta;
        self.idea.update(cx, dt);
        self.base.result = 1;
        1
    }

    fn accept_directive(&mut self, _cx: &mut Cx<'_, W>, _op: &DirectiveOp) -> bool {
        self.base.flags |= 0x2;
        true
    }
}

/// `DelayBehavior` (id 20): idles until its delay has passed, then
/// reports the directive finished.
#[derive(Clone, Debug, PartialEq)]
pub struct DelayBehavior {
    base: BehaviorBase,
    /// +0x2c the idle idea.
    pub idea: IdleIdea,
    /// +0x40 the delay.
    pub delay: f32,
    /// +0x44 the time spent.
    pub elapsed: f32,
}

impl DelayBehavior {
    /// `DelayBehavior::DelayBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            idea: IdleIdea::default(),
            delay: 0.0,
            elapsed: 0.0,
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for DelayBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let ok = self.base.bind(owner_id);
        if ok {
            Idea::<W>::bind(&mut self.idea, owner_id);
            self.delay = 0.0;
            self.elapsed = 0.0;
        }
        ok
    }

    fn engage(&mut self, _cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.elapsed = 0.0;
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.idea.commit(cx);
        self.base.disengage(cx.state);
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        self.base.mode_from_flags()
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if !self.base.bound {
            self.base.result = 3;
            return 3;
        }
        let dt = self.base.tick_delta;
        self.idea.update(cx, dt);
        let sum = e(self.elapsed) + e(self.base.tick_delta);
        self.elapsed = r(sum);
        if sum > e(self.delay) {
            // The reference parks a sentinel (-1e8) so the delay does not
            // finish twice.
            self.elapsed = f32::from_bits(0xccbe_bc20);
            cx.set_directive_result(ids::DELAY, 0);
        }
        self.base.result = 1;
        1
    }

    fn accept_directive(&mut self, _cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        // The same op word goes to both fields, as in the reference.
        let value = f32::from_bits(op_dword(op, 0x0c));
        self.delay = value;
        self.base.flags |= 0x2;
        self.elapsed = value;
        true
    }
}

/// `CruiseBehavior` (id 12): cruise on while current.
#[derive(Clone, Debug, PartialEq)]
pub struct CruiseBehavior {
    base: BehaviorBase,
}

impl CruiseBehavior {
    /// `CruiseBehavior::CruiseBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for CruiseBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        cx.engage_cruise(true);
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        cx.engage_cruise(false);
        self.base.disengage(cx.state);
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        self.base.mode_from_flags()
    }

    fn update(&mut self, _cx: &mut Cx<'_, W>) -> i32 {
        self.base.result = if self.base.bound { 1 } else { 3 };
        self.base.result
    }
}

/// `DrasticEvadeBehavior` (id 18): a timed nudge (kind 0) or full reverse
/// (kind 1).
#[derive(Clone, Debug, PartialEq)]
pub struct DrasticEvadeBehavior {
    base: BehaviorBase,
    /// +0x2c the time spent.
    pub timer: f32,
    /// +0x30 how long (4 until a directive sets it).
    pub duration: f32,
    /// +0x34 the nudge of kind 0 (x and y).
    pub offset: Vector,
    /// +0x40 the kind.
    pub kind: u32,
}

impl DrasticEvadeBehavior {
    /// `DrasticEvadeBehavior::DrasticEvadeBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            timer: 0.0,
            duration: 4.0,
            offset: Vector::ZERO,
            kind: 0,
        }
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for DrasticEvadeBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn engage(&mut self, _cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.timer = 0.0;
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.disengage(cx.state);
        self.base.flags |= 0x2;
    }

    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = 2;
        if self.base.flags & 0x2 != 0 {
            result = 0;
        }
        if self.base.flag29 != 0 {
            result = 3;
        }
        self.base.mode = result;
        result
    }

    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let result = match self.kind {
            0 => {
                let timer = e(self.base.tick_delta) + e(self.timer);
                self.timer = r(timer);
                if timer < e(self.duration) {
                    let nudge = Vector::new(self.offset.x, self.offset.y, 0.0);
                    cx.world.command(cx.ship, AiCommand::SetNudge(nudge));
                    1
                } else {
                    cx.world.command(cx.ship, AiCommand::SetNudge(Vector::ZERO));
                    2
                }
            }
            1 => {
                let timer = e(self.base.tick_delta) + e(self.timer);
                self.timer = r(timer);
                if timer < e(self.duration) {
                    cx.set_throttle(-1.0, 0);
                    1
                } else {
                    cx.set_throttle(0.0, 0);
                    2
                }
            }
            _ => 3,
        };
        self.base.result = result;
        result
    }

    fn accept_directive(&mut self, _cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        self.kind = op_dword(op, 0x0c);
        if self.kind == 0 {
            self.offset = Vector::new(
                f32::from_bits(op_dword(op, 0x10)),
                f32::from_bits(op_dword(op, 0x14)),
                f32::from_bits(op_dword(op, 0x18)),
            );
        }
        self.base.flags |= 0x2;
        self.duration = f32::from_bits(op_dword(op, 0x1c));
        true
    }
}
