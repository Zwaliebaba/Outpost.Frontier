//! The instant trade lane behaviour (`InstantTradeLaneBehavior`,
//! behaviour 9, `Common/common_itlbehavior.cpp`,
//! `include/common_itlbehavior.h`): a ship put into a trade lane at once,
//! without flying to the ring (a scripted or populator ship).
//!
//! The states of `State30`: 0 the lane resolved (`get_lane_start`, unless
//! the directive named the exit ring), 3 into the lane (`go_tradelane`),
//! 4 in the lane, 5 and 6 done (directive result 0 and 2). The lane's end
//! comes as a manager event (slot 22).

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, ids};
use super::gun_convergence::LeaderNotification;
use super::world::AiCommand;

/// `InstantTradeLaneBehavior` (0x5c bytes), field by field.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InstantTradeLaneBehavior {
    base: BehaviorBase,
    /// +0x30 the state.
    pub state: i32,
    /// +0x34 the entrance ring (`SubObjPtr34`).
    pub entrance: Option<u32>,
    /// +0x38 the exit ring (`LanePtr38`).
    pub lane: Option<u32>,
    /// +0x3c the lane's start (`get_lane_start`).
    pub lane_start: Vector,
    /// +0x48 its direction.
    pub lane_end: Vector,
    /// +0x54 in the lane.
    pub in_lane: u8,
    /// +0x55 cleared with the others.
    pub flag_55: u8,
    /// +0x56 a lane request is out.
    pub flag_56: u8,
    /// +0x58 the speed.
    pub speed: f32,
}

impl InstantTradeLaneBehavior {
    /// `InstantTradeLaneBehavior::InstantTradeLaneBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            ..Self::default()
        }
    }

    /// `TurnOff`: out of cruise, back to state 0.
    pub fn turn_off<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        cx.set_cruise_lock(0, 3);
        self.state = 0;
        self.base.engage();
        cx.engage_cruise(false);
    }

    /// The entrance's live id.
    fn entrance_id<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> u32 {
        cx.view_of(self.entrance).map_or(0, |v| v.id)
    }

    /// `HandleTargetEvent` (slot 22): the event of tag 4 for this ship
    /// (the lane's end) finishes the behaviour and releases the lane.
    pub fn handle_target_event<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        tag: i32,
        payload_flag: u8,
        ship_id: u32,
    ) {
        if tag != 4 || payload_flag != 0 {
            return;
        }
        if cx.own().map_or(0, |v| v.id) != ship_id {
            return;
        }
        self.state = 5;
        if self.entrance.is_none() {
            return;
        }
        let id = self.entrance_id(cx);
        cx.cancel_request(2, id, 0);
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for InstantTradeLaneBehavior {
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

    /// (1) `SetContext`: the lane state cleared.
    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let ok = self.base.bind(owner_id);
        if ok {
            self.flag_55 = 0;
            self.flag_56 = 0;
            self.in_lane = 0;
            self.entrance = None;
            self.lane = None;
            self.speed = 0.0;
        }
        ok
    }

    /// (3) `TurnOff`.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.turn_off(cx);
    }

    /// (4) `RequestExitTradeLane`: out of the lane, the behaviour done.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        let was_in_lane = self.in_lane != 0;
        self.flag_55 = 0;
        self.flag_56 = 0;
        self.base.flags |= 0x1;
        if was_in_lane {
            self.in_lane = 0;
            cx.world.command(cx.ship, AiCommand::RequestExitTradeLane);
        }
        self.base.disengage(cx.state);
        self.entrance = None;
        cx.set_cruise_lock(0, 3);
    }

    /// (5) `UpdateMode`: 1 in the lane or at a lane ring, else 2; 3 while
    /// +0x29.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut mode = 2;
        if self.base.bound {
            if self.in_lane != 0 {
                mode = 1;
            }
            if self.entrance.is_some()
                && cx
                    .view_of(self.entrance)
                    .is_some_and(|v| v.type_flags & 0x80 != 0)
            {
                mode = 1;
            }
        }
        if self.base.flag29 != 0 {
            mode = 3;
        }
        self.base.mode = mode;
        mode
    }

    /// (7) `Update`.
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if self.base.flags & 0x1 != 0 {
            self.base.result = 2;
            return 2;
        }
        if !self.base.bound {
            self.base.result = 3;
            return 3;
        }
        match self.state {
            0 => {
                if self.lane.is_none() {
                    let ring = self.entrance.and_then(|id| cx.world.handle_of(id));
                    let answer = cx.world.lane_start(cx.ship, ring, None);
                    if let Some(start) = answer.start {
                        self.lane_start = start;
                    }
                    if let Some(dir) = answer.direction {
                        self.lane_end = dir;
                    }
                    if answer.result != 0 {
                        self.base.result = 3;
                        return 3;
                    }
                    self.lane = answer.lane.and_then(|h| cx.world.inspect(h)).map(|v| v.id);
                }
                self.base.result = 1;
                self.state = 3;
                self.flag_56 = 1;
                self.flag_55 = 0;
                1
            }
            3 => {
                let entrance = self.entrance.and_then(|id| cx.world.handle_of(id));
                let exit = self.lane.and_then(|id| cx.world.handle_of(id));
                let refused = cx.world.command(
                    cx.ship,
                    AiCommand::GoTradeLane {
                        entrance,
                        exit,
                        fast_exit: true,
                        speed: self.speed,
                    },
                ) != 0;
                if refused {
                    self.base.result = 1;
                    self.state = 6;
                    return 1;
                }
                if let Some(own) = cx.own() {
                    cx.state.leader_notes.push(LeaderNotification {
                        leader: own.id,
                        extra: 0,
                        duration: 0.0,
                    });
                }
                self.base.result = 0;
                self.in_lane = 1;
                self.state = 4;
                0
            }
            5 | 6 => {
                let result = if self.state == 6 { 2 } else { 0 };
                self.base.flags |= 0x1;
                Behavior::<W>::disengage(self, cx);
                cx.set_directive_result(ids::INSTANT_TRADE_LANE, result);
                self.base.result = 2;
                2
            }
            _ => {
                self.base.result = 1;
                1
            }
        }
    }

    /// (10) `AttachDirective`: the two rings and the speed.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        self.base.flags &= !0x1;
        let (entrance, exit, speed) = match op {
            DirectiveOp::InstantTradeLane(t) => (t.entrance, t.exit, t.speed),
            _ => (0, 0, 0.0),
        };
        let verify = |id: u32| {
            (id != 0)
                .then(|| cx.view_of(Some(id)).map(|v| v.id))
                .flatten()
        };
        self.entrance = verify(entrance);
        self.lane = verify(exit);
        self.speed = speed;
        if self.entrance.is_some() {
            self.base.flags |= 0x2;
        } else {
            self.base.flags &= !0x2;
        }
        self.entrance.is_some()
    }

    /// `OnResult`: a lane granted goes back to state 3; refused, done.
    fn on_request_result(
        &mut self,
        cx: &mut Cx<'_, W>,
        _kind: i32,
        _response: i32,
        arg: i32,
    ) -> bool {
        self.flag_55 = 0;
        if arg != -1 {
            self.flag_56 = 1;
            self.state = 3;
            return false;
        }
        self.state = 6;
        if self.flag_56 != 0 {
            let id = self.entrance_id(cx);
            cx.cancel_request(2, id, 0);
            self.flag_55 = 0;
            self.flag_56 = 0;
        }
        if self.in_lane != 0 {
            self.in_lane = 0;
            self.state = 6;
        }
        false
    }
}
