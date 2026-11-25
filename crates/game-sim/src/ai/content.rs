//! The behaviour manager's content callback (`pub::AI::ContentCallback`,
//! `include/common_pub.h:1254-1330`) as handles (D710, record 0008 point
//! 4): the manager keeps the owner's handle of the callback and its
//! creation type (`set_content_callback`); the reports it would make
//! inside the behaviour tick are [`CombatEvent`]s that the world queues in
//! order with the AI's requests of the server (`ServerRequest::Content`)
//! and hands over after the tick; the one answer the behaviours wait for,
//! `request_flee_destination`, is a [`FleeAnswer`] the server gives
//! before the ship's tick ([`super::world::AiWorld::content_answer`]).

use neuron_core::math::Vector;

/// A report to the content callback (`pub::AI::ContentCallback`, ten
/// slots), queued for the manager's owner; only made while a callback is
/// installed and, for the follow, tether and lead events, under
/// `IBehaviorManager`'s own conditions.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum CombatEvent {
    /// Slot 7, `combat_state_notification(kind, owner, target)`: kind 0
    /// for attack state 0 (with the ship's target id or 0), 2 for state 1,
    /// 4 for state 2.
    State {
        /// The `CSNType`.
        kind: i32,
        /// The ship's id.
        owner: u32,
        /// The target id, 0 for none.
        target: u32,
    },
    /// Slot 8, `combat_drift_event`: the ship drifted too far from its
    /// anchor.
    Drift,
    /// Slot 3, `ship_in_sights_event(target)`.
    InSights(u32),
    /// Slot 0, `follow_event(Own, Leader, Status, Distance)`
    /// (`IBehaviorManager::NotifyFollowEvent`): the follow behaviour's
    /// leader crossed its range (status 1 in range, 2 out of it).
    Follow {
        /// The follower.
        own: u32,
        /// The leader.
        leader: u32,
        /// `FollowStatusType`.
        status: i32,
        /// The range (status 1) or the speed parameter (status 2).
        distance: f32,
    },
    /// Slot 1, `tether_object_event(Own, Watched, Status, Position)`
    /// (`IBehaviorManager::NotifyTetherEvent`): the proximity watcher's
    /// zone change (1 entered the near zone, 2 the far zone, 3 left it).
    Tether {
        /// The ship.
        own: u32,
        /// The watched object.
        watched: u32,
        /// `TetherStatusType`.
        status: i32,
        /// The reference point.
        position: Vector,
    },
    /// Slot 2, `lead_object_event(Own, Watched, Progress, Value, Position)`
    /// (`GuideBehavior::NotifyManagerGuideEvent`): one of the guide's
    /// progress events 1 to 8.
    Lead {
        /// The guide.
        own: u32,
        /// The guided object.
        watched: u32,
        /// `LeadProgressType`.
        progress: i32,
        /// The clearance.
        value: f32,
        /// The guide's point.
        position: Vector,
    },
    /// Slot 6, `scan_state_change(bScanning)`.
    ScanState(bool),
    /// Slot 4, `request_flee_destination(Own, Reason, ...)` as the flee
    /// asked it: the answer the flee used ([`FleeAnswer`], `None` for
    /// false) was given before the tick; the callback's own effects (a
    /// guide's state, the NPC's active directive, `FleeRequest`) follow
    /// after the tick.
    FleeRequested {
        /// The fleeing ship.
        own: u32,
        /// `FleeReason`.
        reason: i32,
        /// The answer the flee took.
        answer: Option<FleeAnswer>,
    },
}

/// The four out-parameters of `request_flee_destination`, as the flee
/// hands them in (type 0, the zero point, id 0, radius 500) and the
/// callback leaves them.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FleeAnswer {
    /// `FleeDestinationType`: 1 an object, 2 a dock, 3 a point.
    pub kind: i32,
    /// The point (type 3).
    pub position: Vector,
    /// The object (types 1 and 2).
    pub param: u32,
    /// The radius.
    pub radius: f32,
}

impl Default for FleeAnswer {
    /// The values `FleeBehavior::Perform` passes in.
    fn default() -> Self {
        Self {
            kind: 0,
            position: Vector::ZERO,
            param: 0,
            radius: 500.0,
        }
    }
}
