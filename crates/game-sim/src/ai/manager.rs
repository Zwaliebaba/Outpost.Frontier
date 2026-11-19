//! The behaviour manager (`IBehaviorManager`, `include/common_iface.h`,
//! `Common/common_behaviormanager.cpp`): one per AI-driven equipped
//! object. It owns the 21 behaviours, the state graph that weighs the
//! transitions between them, the current directive, and the system
//! manager's parts that the behaviours command the ship through.
//!
//! [`ManagerState`] holds the manager's fields under the names `ai.jsonl`
//! gives them (`behavior`, `state_graph`, `directive`, `maneuver_mask`,
//! `lock`, `dirty`, `suppressed`, `timers`, `in_sights`, `f118`, `f11c`,
//! `f120`, `score_w`, `score_b`), so a replay compares them directly.
//! [`BehaviorManager`] adds the behaviours and runs
//! [`BehaviorManager::update_behavior_tick`], the reference's
//! `UpdateBehaviorTick`, in its order.
//!
//! # The system manager
//!
//! The reference's manager builds a `SystemManager` (0x195c bytes: the
//! target manager, gun groups and fire control, the timer table that
//! builds the AI target list, the repair manager, the formation-leader
//! block, the thrust manager, the personality). The port has the thrust
//! manager ([`super::thrust`]), the personality and the combat parts
//! ([`super::system::SystemManager`]); the repair manager is not ported.

use std::sync::Arc;

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, CancelRequest, Cx, DirectiveOp, EvaluateResult, ids};
use super::behaviors::{UnportedBehavior, create_behavior};
use super::fp::{e, r};
use super::gun_convergence::LeaderNotification;
use super::intercept::{is_aligned_and_outside_heading_cone, is_within_intercept_range_of_iface};
use super::pathfind::PathfindManager;
use super::personality::Personality;
use super::state_graph::{
    BEHAVIOR_COUNT, BehaviorChoice, BehaviorScore, StateGraph, StateGraphDb, choose_behavior,
};
use super::system::SystemManager;
use super::target_manager::CombatEvent;
use super::thrust::ThrustManager;
use super::world::{AiRand, ObjectView, StrafeDir};

/// One queued notification (`NotificationNode`).
#[derive(Clone, Debug, PartialEq)]
pub struct Notification {
    /// +0x08 0 re-runs `QueueNotification`, 1 `InsertNotification`.
    pub mode: u32,
    /// +0x0c the first watched object.
    pub a: Option<u32>,
    /// +0x14 the second watched object.
    pub b: Option<u32>,
    /// +0x1c the notification code.
    pub extra: u32,
    /// +0x20 the countdown.
    pub timer: f32,
}

/// A notification that reaches another object's manager: the immediate
/// path of `InsertNotification` on `target` with code `extra`. The world
/// that owns the managers delivers it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NotificationOut {
    /// `QueueNotification`'s object (the followers of it are notified),
    /// or `InsertNotification`'s object.
    pub target: u32,
    /// Whether it is `QueueNotification` (true) or `InsertNotification`.
    pub to_followers: bool,
    /// The code.
    pub extra: u32,
}

/// The manager's fields (`IBehaviorManager`), with the system manager's
/// ported parts.
#[derive(Clone, Debug, PartialEq)]
pub struct ManagerState {
    /// +0x04 the state graph.
    pub state_graph: Option<Arc<StateGraph>>,
    /// +0x10 the score table of the last re-evaluation.
    pub scores: [BehaviorScore; BEHAVIOR_COUNT],
    /// +0xB8 the player's disabled manoeuvres (bit `index + 1`).
    pub maneuver_mask: u32,
    /// +0xBC the player's manoeuvres are locked.
    pub lock: bool,
    /// +0xC0 the current behaviour, -1 for none.
    pub behavior: i32,
    /// +0xC4 the directive: kind (0 none, 1 a behaviour, 2 waiting for a
    /// player manoeuvre), priority, behaviour.
    pub directive: [i32; 3],
    /// +0xD0 `BehaviorTimerA`, the 3-second re-evaluation timer.
    pub timer_a: f32,
    /// +0xD4 `BehaviorTimerB`, the time handed to the behaviour.
    pub timer_b: f32,
    /// +0xD8 `BehaviorDirty`: the behaviour ticks at the next update.
    pub dirty: bool,
    /// +0xDC `BehaviorTimerC`, the accumulated time slot 5 receives.
    pub timer_c: f32,
    /// +0xE0 `AttackOverrideTimer`.
    pub attack_override_timer: f32,
    /// +0xE4 `AttackOverridePeriod`.
    pub attack_override_period: f32,
    /// +0xE8 `InSightsTargetId`.
    pub in_sights: u32,
    /// +0xF8 the reported scan state (`scan_state_change`).
    pub field_f8: bool,
    /// +0xF9 `LevelCameraFlag`.
    pub level_camera: bool,
    /// +0x110 a content callback is installed.
    pub content_callback: bool,
    /// +0x118 the directive result word.
    pub f118: i32,
    /// +0x11C the finished behaviour, -1 for none.
    pub f11c: i32,
    /// +0x120 the fallback behaviour (6, the player's 19).
    pub f120: i32,
    /// +0x124 the player's last steering direction.
    pub last_direction: Vector,
    /// +0x130 `ShipUpDirFlag`.
    pub ship_up_flag: bool,
    /// +0x134 the ship's up direction.
    pub ship_up: Vector,
    /// +0x140 the camera's up direction.
    pub camera_up: Vector,
    /// +0x150 the player is steering.
    pub user_turning: bool,
    /// +0x154 `CameraLevelStatus` (the camera mode).
    pub camera_level_status: i32,
    /// +0x15c the notification queue.
    pub notifications: Vec<Notification>,
    /// +0x164 1 minus the clamped turn sensitivity.
    pub turn_sensitivity: f32,
    /// +0x168 the docking probe's head-tracking flag.
    pub field_168: bool,
    /// +0x169 `DockingPortFlag`.
    pub docking_port_flag: bool,
    /// +0x16C `DockingPortStatus`.
    pub docking_port_status: i32,
    /// +0x170 `TickSuppressedFlag`.
    pub suppressed: bool,
    /// +0x1C8 bit 0: the target branch of the tick runs.
    pub flags_1c8: u8,
    /// +0x0C the pathfinder.
    pub pathfind: PathfindManager,
    /// `SystemManager::Thrust`.
    pub thrust: ThrustManager,
    /// `SystemManager::PersonalityData`.
    pub personality: Personality,
    /// The system manager's combat parts.
    pub sys: SystemManager,
    /// The directive results the callbacks would receive (`f11c`,
    /// `f118`), in order; the owner drains them.
    pub completed: Vec<(i32, i32)>,
    /// Notifications for other objects' managers; the owner drains them.
    pub outbox: Vec<NotificationOut>,
    /// The formation leader's notifications a behaviour asked for
    /// (`NotifyFormationLeader1`, `NotifyFormationLeader2`); the manager
    /// queues them when the behaviour's call returns.
    pub leader_notes: Vec<LeaderNotification>,
    /// How often a behaviour the port does not have yet answered a poll
    /// with its stand-in (a replay counts the ticks whose result may
    /// depend on one). Not a field of the reference.
    pub stand_in_polls: u32,
    /// The ship's pending request (`Ship::PendingRequest`, `+0x9c`): the
    /// index of the behaviour whose `OnResult` the answer reaches. The
    /// server's ship keeps it; the port keeps it with the manager, which
    /// owns the behaviour (D186).
    pub pending_request: Option<i32>,
    /// `Ship::Field_0x98`: a request is in progress, and a new one is
    /// refused.
    pub request_busy: bool,
}

impl ManagerState {
    /// The manager's fields after its constructor, before the
    /// behaviours and the three random phase offsets.
    #[must_use]
    pub fn new(is_player: bool) -> Self {
        Self {
            state_graph: None,
            scores: [BehaviorScore::default(); BEHAVIOR_COUNT],
            maneuver_mask: 0,
            lock: false,
            behavior: 0,
            directive: [0, 0, -1],
            timer_a: 0.0,
            timer_b: 0.0,
            dirty: false,
            timer_c: 0.0,
            attack_override_timer: 0.0,
            attack_override_period: 1.0,
            in_sights: 0,
            field_f8: true,
            level_camera: true,
            content_callback: false,
            f118: 0,
            f11c: -1,
            f120: if is_player { 0x13 } else { 6 },
            last_direction: Vector::ZERO,
            ship_up_flag: false,
            ship_up: Vector::new(0.0, 1.0, 0.0),
            camera_up: Vector::new(0.0, 1.0, 0.0),
            user_turning: false,
            camera_level_status: 0,
            notifications: Vec::new(),
            turn_sensitivity: 0.75,
            field_168: true,
            docking_port_flag: false,
            docking_port_status: 0,
            suppressed: false,
            flags_1c8: 1,
            pathfind: PathfindManager::default(),
            thrust: ThrustManager::new(is_player),
            personality: Personality::default(),
            sys: SystemManager::default(),
            completed: Vec::new(),
            outbox: Vec::new(),
            leader_notes: Vec::new(),
            stand_in_polls: 0,
            pending_request: None,
            request_busy: false,
        }
    }

    /// The state graph's id, -1 for none.
    #[must_use]
    pub fn state_graph_id(&self) -> i32 {
        self.state_graph.as_ref().map_or(-1, |g| g.id)
    }

    /// The five timers in `ai.jsonl`'s order.
    #[must_use]
    pub fn timers(&self) -> [f32; 5] {
        [
            self.timer_a,
            self.timer_b,
            self.timer_c,
            self.attack_override_timer,
            self.attack_override_period,
        ]
    }

    /// `ThrustManager::CheckSteeringGate`.
    #[must_use]
    pub fn steering_gate(&self, is_player: bool) -> bool {
        if self.camera_level_status == 1 && self.level_camera {
            return true;
        }
        if is_player && (self.behavior == 10 || self.behavior == 7) {
            return true;
        }
        self.thrust.auto_level
    }
}

/// `rand() * (1/32768) * period`, one rounding at the store.
fn phase(rand: i32, period: f32) -> f32 {
    r(e(rand as f32) * e(1.0 / 32768.0) * e(period))
}

/// The behaviour manager of one object.
pub struct BehaviorManager<W: AiEnv + ?Sized> {
    /// The fields.
    pub state: ManagerState,
    behaviors: Vec<Box<dyn Behavior<W>>>,
    /// The stand-in that takes a behaviour's entry while it is called.
    spare: Box<dyn Behavior<W>>,
}

impl<W: AiEnv + ?Sized> core::fmt::Debug for BehaviorManager<W> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("BehaviorManager")
            .field("state", &self.state)
            .finish_non_exhaustive()
    }
}

impl<W: AiEnv + ?Sized> BehaviorManager<W> {
    /// `IBehaviorManager::IBehaviorManager(Owner)` for `ship`: the 21
    /// behaviours bound to it, behaviour 0 current, and the three random
    /// phase offsets of timers A and B and the attack-override timer, in
    /// the reference's draw order.
    ///
    /// The system manager's initialisation (`InitSystemManager`) comes
    /// first and draws too: each gun's timing, then the auto-turret scan
    /// phase.
    pub fn new(world: &mut W, ship: W::Handle) -> Self {
        let view = world.inspect(ship);
        let is_player = view.as_ref().is_some_and(|v| v.is_player);
        let owner_id = view.as_ref().map(|v| v.id);
        let mut state = ManagerState::new(is_player);
        // `new PathfindManager(this)`: three draws of its own, then
        // `InitSystemManager`.
        state.pathfind = PathfindManager::new(world, ship);
        let personality = state.personality;
        state.sys.init(world, ship, &personality);
        let mut behaviors: Vec<Box<dyn Behavior<W>>> = Vec::with_capacity(BEHAVIOR_COUNT);
        for index in 0..BEHAVIOR_COUNT {
            let mut b = create_behavior::<W>(i32::try_from(index).unwrap_or(0));
            b.bind(owner_id);
            behaviors.push(b);
        }
        let mut manager = Self {
            state,
            behaviors,
            spare: Box::new(UnportedBehavior::new(-1)),
        };
        manager.attach_all(world, ship);
        let mut state = manager.state;
        let behaviors = manager.behaviors;
        state.behavior = 0;
        state.timer_a = phase(world.rand(), 3.0);
        state.timer_c = 0.0;
        state.attack_override_period = 1.0;
        state.timer_b = phase(world.rand(), 0.2);
        state.attack_override_timer = phase(world.rand(), 1.0);
        state.attack_override_period = 1.0;
        Self {
            state,
            behaviors,
            spare: Box::new(UnportedBehavior::new(-1)),
        }
    }

    /// The manager over given fields and fresh, bound behaviours, drawing
    /// nothing (the replay rebuilds a manager this way).
    #[must_use]
    pub fn from_state(state: ManagerState, owner_id: Option<u32>) -> Self {
        let mut behaviors: Vec<Box<dyn Behavior<W>>> = Vec::with_capacity(BEHAVIOR_COUNT);
        for index in 0..BEHAVIOR_COUNT {
            let mut b = create_behavior::<W>(i32::try_from(index).unwrap_or(0));
            b.bind(owner_id);
            behaviors.push(b);
        }
        Self {
            state,
            behaviors,
            spare: Box::new(UnportedBehavior::new(-1)),
        }
    }

    /// Every behaviour's [`Behavior::attach`] against the world (the
    /// world-dependent half of `SetContext`); a replay that builds the
    /// manager with [`Self::from_state`] calls it once it has a world.
    pub fn attach_all(&mut self, world: &mut W, ship: W::Handle) {
        for b in &mut self.behaviors {
            let mut cx = Cx::new(&mut *world, ship, &mut self.state);
            b.attach(&mut cx);
        }
    }

    /// The behaviour with an index.
    #[must_use]
    pub fn behavior(&self, index: i32) -> Option<&dyn Behavior<W>> {
        usize::try_from(index)
            .ok()
            .and_then(|i| self.behaviors.get(i))
            .map(|b| &**b)
    }

    /// The behaviour with an index, mutably.
    pub fn behavior_mut(&mut self, index: i32) -> Option<&mut dyn Behavior<W>> {
        usize::try_from(index)
            .ok()
            .and_then(|i| self.behaviors.get_mut(i))
            .map(|b| &mut **b as &mut dyn Behavior<W>)
    }

    /// Whether the behaviour with an index is ported.
    #[must_use]
    pub fn is_ported(&self, index: i32) -> bool {
        self.behavior(index).is_some_and(|b| b.ported())
    }

    fn slot(index: i32) -> Option<usize> {
        usize::try_from(index).ok().filter(|&i| i < BEHAVIOR_COUNT)
    }

    /// Runs `f` on behaviour `index` with a context.
    fn with<T>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        index: i32,
        f: impl FnOnce(&mut dyn Behavior<W>, &mut Cx<'_, W>) -> T,
    ) -> Option<T> {
        let i = Self::slot(index)?;
        // The behaviour leaves the table for the call, so that it can
        // reach the others (`Cx::with_peer`).
        std::mem::swap(&mut self.behaviors[i], &mut self.spare);
        let mut cx = Cx {
            world: &mut *world,
            ship,
            state: &mut self.state,
            peers: Some(&mut self.behaviors),
        };
        let out = f(&mut *self.spare, &mut cx);
        std::mem::swap(&mut self.behaviors[i], &mut self.spare);
        // `QueueNotification` from inside the behaviour: nothing the
        // behaviour reads later in the same call depends on it.
        for n in std::mem::take(&mut self.state.leader_notes) {
            self.queue_notification(world, ship, Some(n.leader), n.extra, n.duration);
        }
        Some(out)
    }

    fn is_player(world: &W, ship: W::Handle) -> bool {
        world.inspect(ship).is_some_and(|v| v.is_player)
    }

    /// `SetCurrentBehavior(NewIndex, bForce, DirectiveArg)`: refused for
    /// the player's masked or locked manoeuvres; the player's idle becomes
    /// the fallback with every input released; the outgoing behaviour's
    /// slot 4, the incoming one's slot 10 (with a directive, whose answer
    /// is the result) and slot 3.
    pub fn set_current_behavior(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        new_index: i32,
        force: bool,
        op: Option<&DirectiveOp>,
    ) -> bool {
        let mut effective = new_index;
        if Self::is_player(world, ship) {
            let bit = 2u32.wrapping_shl(new_index as u32 & 31);
            if self.state.maneuver_mask & bit != 0 || self.state.lock {
                return false;
            }
            if new_index == ids::IDLE {
                effective = self.state.f120;
                self.state.thrust.stop(world, ship);
            }
        }
        if self.state.behavior == effective && !force {
            return true;
        }
        let outgoing = self.state.behavior;
        let mut result = true;
        self.with(world, ship, outgoing, |b, cx| b.disengage(cx));
        self.with(world, ship, effective, |b, cx| {
            if let Some(op) = op {
                result = b.accept_directive(cx, op);
            }
            b.engage(cx);
        });
        self.state.behavior = effective;
        result
    }

    /// `NotifyDirectiveCallbacks`: the result goes to the owner.
    fn notify_directive_callbacks(&mut self) {
        self.state
            .completed
            .push((self.state.f11c, self.state.f118));
    }

    /// `set_current_directive(Dir, Op)`.
    pub fn set_current_directive(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        dir: [i32; 3],
        op: Option<&DirectiveOp>,
    ) -> bool {
        self.state.directive = dir;
        if dir[0] == 0 {
            self.state.directive[2] = 0;
            let current = self.state.behavior;
            if let Some(i) = Self::slot(current) {
                self.behaviors[i].cancel_notify();
            }
            let fallback = self.state.f120;
            self.set_current_behavior(world, ship, fallback, false, None);
            // `SetFollowLeader(nullptr)` on the gun convergence block: not
            // ported (the system manager).
            return true;
        }
        if self.state.directive[1] == 1 && dir[0] == 1 {
            self.set_current_behavior(world, ship, dir[2], false, op);
            return true;
        }
        for b in &mut self.behaviors {
            b.cancel_notify();
        }
        if self.state.directive[0] == 1 {
            let target = self.state.directive[2];
            return self.set_current_behavior(world, ship, target, true, op);
        }
        let fallback = self.state.f120;
        self.set_current_behavior(world, ship, fallback, false, None);
        true
    }

    /// `set_state_graph(GraphId, bForce)` from `graphs`.
    pub fn set_state_graph(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        graphs: &StateGraphDb,
        graph_id: i32,
        force: bool,
    ) {
        if let Some(g) = &self.state.state_graph
            && g.id == graph_id
            && !force
        {
            return;
        }
        let view = world.inspect(ship);
        let mut effective = graph_id;
        if view.as_ref().is_some_and(|v| v.is_player) {
            self.state.flags_1c8 &= 0xfe;
            effective = 0;
        }
        // `get_type` into `bForce`'s own slot: an object without a type
        // leaves the flag's value there.
        let kind = view.as_ref().map_or(u32::from(force), |v| v.type_flags);
        if kind & 0xfdff != 0 {
            self.state.flags_1c8 &= 0xfe;
        }
        let new_graph = graphs.get(effective);
        let same = match (&self.state.state_graph, new_graph) {
            (None, None) => true,
            (Some(a), Some(b)) => a.id == b.id,
            _ => false,
        };
        if same {
            return;
        }
        self.state.state_graph = new_graph.map(|g| Arc::new(g.clone()));
        if self.state.state_graph.is_none() {
            // FDUMP "IBehaviorManager: state graph not found in database!"
            return;
        }
        self.set_current_directive(world, ship, [0, 0, -1], None);
    }

    /// `refresh_state_graph`.
    pub fn refresh_state_graph(&mut self, world: &mut W, ship: W::Handle, graphs: &StateGraphDb) {
        if let Some(id) = self.state.state_graph.as_ref().map(|g| g.id) {
            self.set_state_graph(world, ship, graphs, id, true);
        }
    }

    /// `pub::AI::SubmitDirective` on an AI ship (the server's path for a
    /// ship no player owns): a cancel clears the directive; a directive
    /// blocks another (2); an op with a behaviour starts it (1), anything
    /// else is refused (5). The op is validated first.
    pub fn submit_directive(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        op: &DirectiveOp,
        priority: i32,
    ) -> i32 {
        if !validate(op) {
            return 5;
        }
        if matches!(op, DirectiveOp::Cancel) {
            self.set_current_directive(world, ship, [0, priority, -1], Some(op));
            return 1;
        }
        if self.state.directive[0] != 0 {
            return 2;
        }
        match op.behavior() {
            Some(id) if self.set_current_directive(world, ship, [1, priority, id], Some(op)) => 1,
            _ => 5,
        }
    }

    /// `set_directive_priority`: 1 with a directive, 3 without.
    pub fn set_directive_priority(&mut self, priority: i32) -> i32 {
        if self.state.directive[0] != 0 {
            self.state.directive[1] = priority;
            1
        } else {
            3
        }
    }

    /// `set_personality`, then the system manager's refresh from it
    /// (`NotifySystemManagerPersonalityChanged`: the gun groups, which
    /// draw, the timer thresholds, the force-attack flag).
    pub fn set_personality(&mut self, world: &mut W, ship: W::Handle, personality: Personality) {
        self.state.personality = personality;
        self.state
            .sys
            .personality_changed(world, ship, &personality);
    }

    /// `enable_maneuver`: bit `maneuver + 1` (mod 32); -1 refused.
    pub fn enable_maneuver(&mut self, maneuver: i32, enable: bool) -> bool {
        if maneuver == -1 {
            return false;
        }
        let bit = 2u32.wrapping_shl(maneuver as u32 & 0x1f);
        if enable {
            self.state.maneuver_mask &= !bit;
        } else {
            self.state.maneuver_mask |= bit;
        }
        true
    }

    /// `enable_all_maneuvers`.
    pub fn enable_all_maneuvers(&mut self) -> bool {
        self.state.maneuver_mask = 0;
        true
    }

    /// `Ship::request_returned(RequestType, Response, Arg3)` with the
    /// ship's pending callback: the requesting behaviour's `OnResult`; an
    /// answer it does not keep ends the request. Without a pending
    /// callback the answer goes to the player's client in the reference
    /// (not the AI's), so nothing happens here.
    pub fn request_returned(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        kind: i32,
        response: i32,
        arg: i32,
    ) {
        let Some(index) = self.state.pending_request else {
            return;
        };
        let keep = self
            .with(world, ship, index, |b, cx| {
                b.on_request_result(cx, kind, response, arg)
            })
            .unwrap_or(false);
        if !keep {
            self.state.pending_request = None;
            self.state.request_busy = false;
        }
    }

    /// `lock_maneuvers`.
    pub fn lock_maneuvers(&mut self, lock: bool) -> bool {
        self.state.lock = lock;
        true
    }

    /// `set_turn_sensitivity`: clamped to 0..0.9, stored as 1 minus it.
    pub fn set_turn_sensitivity(&mut self, sensitivity: f32) {
        let mut s = sensitivity;
        if !(s > 0.0) {
            s = 0.0;
        } else if !(s < 0.9) {
            s = 0.9;
        }
        self.state.turn_sensitivity = r(1.0 - e(s));
    }

    /// `external_player_evaluate`.
    pub fn external_player_evaluate(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        index: i32,
    ) -> EvaluateResult {
        let bit = 2u32.wrapping_shl(index as u32 & 0x1f);
        if (!self.state.lock || index == self.state.behavior) && self.state.maneuver_mask & bit == 0
        {
            let timer_c = self.state.timer_c;
            return self
                .with(world, ship, index, |b, cx| b.evaluate(cx, timer_c))
                .unwrap_or(EvaluateResult::Skip);
        }
        EvaluateResult::Skip
    }

    /// `allow_head_tracking`.
    #[must_use]
    pub fn allow_head_tracking(&self) -> bool {
        self.behavior(self.state.behavior)
            .is_some_and(|b| b.allow_head_tracking())
    }

    /// `cancel_behavior`.
    pub fn cancel_behavior(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        request: CancelRequest,
    ) -> bool {
        let current = self.state.behavior;
        self.with(world, ship, current, |b, cx| b.cancel_behavior(cx, request))
            .unwrap_or(true)
    }

    /// `get_debugger_target`: the behaviour's target, else the target
    /// manager's (`TargetManager::GetTarget`, the ship's target).
    #[must_use]
    pub fn debugger_target(&self, world: &W, ship: W::Handle) -> Option<u32> {
        if self.state.behavior == -1 {
            return None;
        }
        self.behavior(self.state.behavior)
            .and_then(|b| b.target())
            .or_else(|| world.inspect(ship).and_then(|v| v.target))
    }

    /// The player's inputs that the manager forwards to the current
    /// behaviour (`update_current_behavior_afterburner`, `_brake_reverse`,
    /// `_engage_engine`, `_slide_strafe_burst`, `_throttle`), each marking
    /// the behaviour dirty except the engine toggle.
    pub fn update_current_behavior(&mut self, world: &mut W, ship: W::Handle, input: PlayerInput) {
        let current = self.state.behavior;
        if Self::slot(current).is_none() {
            return;
        }
        if !matches!(input, PlayerInput::EngageEngine(_)) {
            self.state.dirty = true;
        }
        self.with(world, ship, current, |b, cx| match input {
            PlayerInput::Afterburner(on) => b.set_afterburner(cx, on),
            PlayerInput::BrakeReverse(on) => b.brake_reverse(cx, on),
            PlayerInput::EngageEngine(on) => b.engage_engine(cx, on),
            PlayerInput::SlideStrafeBurst(dir) => b.slide_strafe_burst(cx, dir),
            PlayerInput::Throttle(t) => b.set_throttle(cx, t),
        });
    }

    /// `update_current_behavior_cruise`: forwarded while the pathfinder
    /// allows cruise; otherwise the ship is told cruise is blocked
    /// (`slot_6c(4, 0)`).
    pub fn update_current_behavior_cruise(&mut self, world: &mut W, ship: W::Handle, on: bool) {
        let current = self.state.behavior;
        if Self::slot(current).is_none() {
            return;
        }
        if self.state.pathfind.is_cruise_unobstructed() {
            self.state.dirty = true;
            self.with(world, ship, current, |b, cx| b.set_cruise(cx, on));
            return;
        }
        world.command(
            ship,
            super::world::AiCommand::Slot {
                slot: 27,
                ints: vec![4, 0],
                floats: Vec::new(),
            },
        );
    }

    /// `reset_current_behavior_direction`.
    pub fn reset_current_behavior_direction(&mut self, world: &mut W, ship: W::Handle) {
        self.state.dirty = true;
        let current = self.state.behavior;
        self.with(world, ship, current, |b, cx| {
            b.set_direction(cx, Vector::ZERO)
        });
        self.state.last_direction = Vector::ZERO;
    }

    /// `UpdateNotificationQueue`: each node counts down; an expired node
    /// re-runs its operation at once and leaves the queue.
    fn update_notification_queue(&mut self, world: &mut W, ship: W::Handle, dt: f32) {
        let mut kept = Vec::with_capacity(self.state.notifications.len());
        let nodes = std::mem::take(&mut self.state.notifications);
        for mut node in nodes {
            node.timer = r(e(node.timer) - e(dt));
            if node.timer > 0.0 || node.timer.is_nan() {
                kept.push(node);
                continue;
            }
            match node.mode {
                0 => self.queue_now(world, ship, node.a, node.extra),
                1 => self.insert_now(world, ship, node.b, node.extra),
                _ => {}
            }
        }
        // The immediate operations queue no node, so the list is the kept
        // nodes in their order.
        self.state.notifications = kept;
    }

    /// The immediate path of `QueueNotification`: each of the target's
    /// formation followers (at most 16) gets `InsertNotification(Extra)`.
    fn queue_now(&mut self, world: &mut W, ship: W::Handle, target: Option<u32>, extra: u32) {
        let Some(id) = target else {
            return;
        };
        let followers = world
            .handle_of(id)
            .and_then(|h| world.inspect(h))
            .map(|v| v.formation_followers)
            .unwrap_or_default();
        for f in followers.into_iter().take(16) {
            self.insert_now(world, ship, Some(f), extra);
        }
    }

    /// The immediate path of `InsertNotification`: on this ship's own
    /// manager at once ([`Self::receive_notification`]); for another ship
    /// into [`ManagerState::outbox`], which the world that owns the
    /// managers delivers when this call returns, in order (D195).
    fn insert_now(&mut self, world: &mut W, ship: W::Handle, target: Option<u32>, extra: u32) {
        let Some(id) = target else {
            return;
        };
        if world.inspect(ship).is_some_and(|v| v.id == id) {
            self.receive_notification(world, ship, extra);
            return;
        }
        self.state.outbox.push(NotificationOut {
            target: id,
            to_followers: false,
            extra,
        });
    }

    /// What `InsertNotification` does to the target's manager (this one):
    /// 0 ticks the formation behaviour when it is current (slot 7) and
    /// then, as 1, clears the formation block's force-attack flag; 2 sets
    /// it; 3 notifies the ship's own formation followers with 1. The
    /// notifications this queues for other ships land in the outbox.
    pub fn receive_notification(&mut self, world: &mut W, ship: W::Handle, extra: u32) {
        match extra {
            0 | 1 => {
                if extra == 0 && self.state.behavior == ids::FORMATION {
                    self.with(world, ship, ids::FORMATION, |b, cx| {
                        b.update(cx);
                    });
                }
                self.state.sys.formation.force_attack = false;
            }
            2 => self.state.sys.formation.force_attack = true,
            3 => {
                let own = world.inspect(ship).map(|v| v.id);
                self.queue_now(world, ship, own, 1);
            }
            _ => {}
        }
    }

    /// `QueueNotification(Target, Extra, Duration)`: a positive duration
    /// queues it; otherwise each formation follower of the target is
    /// notified now.
    pub fn queue_notification(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        target: Option<u32>,
        extra: u32,
        duration: f32,
    ) {
        if duration > 0.0 {
            self.state.notifications.push(Notification {
                mode: 0,
                a: target,
                b: None,
                extra,
                timer: duration,
            });
            return;
        }
        self.queue_now(world, ship, target, extra);
    }

    /// `InsertNotification(Target, Extra, Duration)`.
    pub fn insert_notification(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        target: Option<u32>,
        extra: u32,
        duration: f32,
    ) {
        if duration > 0.0 {
            let own_id = world.inspect(ship).map(|v| v.id);
            self.state.notifications.push(Notification {
                mode: 1,
                a: own_id,
                b: target,
                extra,
                timer: duration,
            });
            return;
        }
        self.insert_now(world, ship, target, extra);
    }

    /// `IBehaviorManager::UpdateBehaviorTick(Dt)` for `ship`.
    pub fn update_behavior_tick(&mut self, world: &mut W, ship: W::Handle, dt: f32) {
        if self.state.suppressed {
            return;
        }
        let Some(view) = world.inspect(ship) else {
            return;
        };
        // `bKeepCache`: the target list is built at most once in the tick.
        self.state.sys.targets.keep_cache = true;
        self.tick_body(world, ship, dt, &view);
        self.state.sys.targets.keep_cache = false;
    }

    /// The system manager's update of the tick (`UpdateSystemManager`,
    /// `SyncFormationLeader`), then the target branch (the attack-override
    /// timer, fire control, the in-sights report) and the auto turrets.
    fn update_system(&mut self, world: &mut W, ship: W::Handle, dt: f32, view: &ObjectView) {
        let personality = self.state.personality;
        let p = &personality;
        self.state.sys.update_before_thrust(world, ship, p, dt);
        self.state.thrust.cruise_unobstructed = self.state.pathfind.is_cruise_unobstructed();
        if view.cruise.is_some_and(|c| c.0) && !self.state.thrust.cruise_unobstructed {
            self.state.thrust.engage_cruise(world, ship, false);
        }
        let notes = self.state.sys.update_after_thrust(world, ship, p, dt);
        for n in notes {
            self.queue_notification(world, ship, Some(n.leader), n.extra, n.duration);
        }
        self.state.sys.formation.sync_formation_leader(world, ship);
        let external_is_player = view.is_player;
        let content_callback = self.state.content_callback;
        if self.state.flags_1c8 & 1 != 0 {
            let sum = e(dt) + e(self.state.attack_override_timer);
            self.state.attack_override_timer = r(sum);
            if sum > e(self.state.attack_override_period) {
                self.state.attack_override_timer =
                    r(e(self.state.attack_override_timer) % e(self.state.attack_override_period));
                let directive = self.state.directive;
                self.state.sys.update_attack_target_override(
                    world,
                    ship,
                    p,
                    content_callback,
                    directive,
                );
            }
            let target = world.inspect(ship).and_then(|v| v.target);
            if target.is_some() {
                self.state.sys.run_fire_control_tick(world, ship, p, target);
                // The in-sights report: with a content callback, an AI
                // ship whose target is ahead within its -0.7 cone and
                // 600 m reports it once (`ship_in_sights_event`); any
                // failed test clears the id.
                let own = world.inspect(ship);
                let target_view = target
                    .and_then(|id| world.handle_of(id))
                    .and_then(|h| world.inspect(h));
                let in_sights = content_callback
                    && own.as_ref().is_some_and(|v| !v.is_player)
                    && own.as_ref().is_some_and(|v| {
                        is_aligned_and_outside_heading_cone(v, target_view.as_ref(), -0.7)
                    })
                    && match (&own, &target_view) {
                        (Some(v), Some(t)) => is_within_intercept_range_of_iface(v, t, 600.0),
                        _ => false,
                    };
                if in_sights {
                    let id = target_view.as_ref().map_or(0, |t| t.id);
                    if self.state.in_sights != id {
                        self.state.in_sights = id;
                        self.state.sys.events.push(CombatEvent::InSights(id));
                    }
                } else {
                    self.state.in_sights = 0;
                }
            }
        }
        if !external_is_player {
            let directive = self.state.directive;
            self.state
                .sys
                .auto_turrets(world, ship, p, content_callback, directive, dt);
        }
        let behavior = self.state.behavior;
        let scan = super::pathfind::ScanContext {
            scanner: self.state.sys.timers.scanner,
            keep: self.state.sys.targets.keep_cache,
        };
        self.state
            .pathfind
            .update(&mut self.state.thrust, world, ship, behavior, dt, scan);
    }

    /// `UpdateBehaviorTick` after the entry tests.
    #[allow(clippy::too_many_lines)]
    fn tick_body(&mut self, world: &mut W, ship: W::Handle, dt: f32, view: &ObjectView) {
        self.update_notification_queue(world, ship, dt);
        let Some(graph) = self.state.state_graph.clone() else {
            return;
        };
        self.update_system(world, ship, dt, view);
        if view.is_player && len_sq(self.state.last_direction) != 0.0 {
            let dir = self.state.last_direction;
            self.update_current_behavior_direction(world, ship, dir);
        }
        if self.state.content_callback {
            let d = self.state.directive;
            let flag = self.state.sys.attack_state() == 2
                && self.state.behavior == 2
                && (d[0] == 0 || d[1] == 1);
            if flag != self.state.field_f8 {
                self.state.field_f8 = flag;
            }
        }

        let mut wrapped = false;
        let mut reevaluate = false;
        let new_c = r(e(dt) + e(self.state.timer_c));
        let new_a_ext = e(dt) + e(self.state.timer_a);
        let new_a = r(new_a_ext);
        self.state.timer_a = new_a;
        self.state.timer_b = r(e(dt) + e(self.state.timer_b));
        self.state.timer_c = new_c;
        if new_a_ext >= 3.0 {
            self.state.timer_a = r(e(new_a) % 3.0);
            wrapped = true;
            reevaluate = true;
        }
        let armed = self.state.directive[0] != 0 && self.state.directive[1] == 0;
        let mut current_is_idle = false;
        if self.state.behavior == ids::IDLE {
            wrapped = true;
            self.state.timer_a = 0.0;
            reevaluate = true;
            current_is_idle = true;
        }
        let current = self.state.behavior;
        if Self::slot(current).is_none() {
            return;
        }
        if wrapped {
            let polled = self
                .with(world, ship, current, |b, cx| b.poll(cx, new_c))
                .unwrap_or(1);
            if !current_is_idle && armed {
                reevaluate = false;
            }
            if polled == 4 {
                let finished = self.state.behavior;
                self.state.f118 = 2;
                self.state.f11c = finished;
                if finished != -1 {
                    self.state.directive[0] = 0;
                    let fallback = self.state.f120;
                    self.set_current_behavior(world, ship, fallback, true, None);
                    self.notify_directive_callbacks();
                }
                self.state.f11c = -1;
                return;
            }
        }
        let status = self.behavior(current).map_or(1, |b| b.status());
        if status == 2 || status == 3 {
            reevaluate = true;
        }
        if status == 0 {
            reevaluate = false;
        }
        if !armed {
            if self.state.sys.timer3_active() && self.state.behavior != ids::FACE {
                reevaluate = true;
            }
            if self.state.sys.missile_reaction(&self.state.personality)
                && self.state.behavior != ids::EVADE
            {
                reevaluate = true;
            }
        }
        if Self::is_player(world, ship) {
            self.player_docking_probe(world, ship, status);
            reevaluate = false;
        }
        if self.state.lock {
            reevaluate = false;
        }
        if reevaluate {
            self.reevaluate(world, ship, &graph);
        }
        let current = self.state.behavior;
        if Self::slot(current).is_some() {
            let tick = self.state.timer_b >= 0.2 || self.state.dirty || reevaluate;
            let timer_b = self.state.timer_b;
            if tick {
                self.with(world, ship, current, |b, cx| {
                    b.base_mut().tick_delta = timer_b;
                    b.update(cx);
                });
                self.state.timer_b = 0.0;
                self.state.dirty = false;
            }
            self.with(world, ship, current, |b, cx| b.end_tick(cx));
        }
        let f11c = self.state.f11c;
        if f11c != -1 && (f11c == self.state.behavior || f11c == ids::LAUNCH) {
            self.state.directive[0] = 0;
            let fallback = self.state.f120;
            self.set_current_behavior(world, ship, fallback, true, None);
            self.notify_directive_callbacks();
        }
        self.state.f11c = -1;
    }

    /// The re-evaluation pass: the candidates' verdicts (slot 5, asked of
    /// each candidate with a weight, in index order), then
    /// [`choose_behavior`] and the transition it picks.
    fn reevaluate(&mut self, world: &mut W, ship: W::Handle, graph: &StateGraph) {
        let current = self.state.behavior;
        let dir = self.state.directive;
        let armed = (dir[0] == 1).then_some(dir[2]);
        let row = usize::try_from(current)
            .ok()
            .and_then(|c| graph.weights.get(c))
            .copied()
            .unwrap_or([0.0; BEHAVIOR_COUNT]);
        let timer_c = self.state.timer_c;
        let mut verdicts = [1; BEHAVIOR_COUNT];
        for (index, &weight) in row.iter().enumerate() {
            let index_i32 = i32::try_from(index).unwrap_or(-1);
            let weight = if armed == Some(index_i32) {
                0.001
            } else {
                weight
            };
            if f64::from(weight) == 0.0 {
                continue;
            }
            verdicts[index] = self
                .with(world, ship, index_i32, |b, cx| b.poll(cx, timer_c))
                .unwrap_or(1);
        }
        let mut scores = self.state.scores;
        let choice = choose_behavior(
            graph,
            usize::try_from(current).unwrap_or(usize::MAX),
            armed,
            &mut scores,
            |i| verdicts[i],
            &mut RandOf(world),
        );
        self.state.scores = scores;
        match choice {
            BehaviorChoice::NoneEligible => {
                if self.state.directive[0] == 1 {
                    self.state.f118 = 2;
                    self.state.f11c = self.state.directive[2];
                    self.state.directive[0] = 0;
                }
                let fallback = self.state.f120;
                if self.state.behavior != fallback {
                    self.set_current_behavior(world, ship, fallback, false, None);
                }
            }
            BehaviorChoice::Certain(chosen) | BehaviorChoice::Drawn(Some(chosen)) => {
                self.set_current_behavior(world, ship, chosen, false, None);
            }
            // D191: the reference reads past the table; no transition.
            BehaviorChoice::Drawn(None) => {}
        }
    }

    /// The player's docking probe of the tick (`Opaque168`,
    /// `DockingPortFlag`, `DockingPortStatus`).
    fn player_docking_probe(&mut self, world: &mut W, ship: W::Handle, status: i32) {
        if status == 2 || status == 3 {
            let fallback = self.state.f120;
            self.set_current_behavior(world, ship, fallback, true, None);
        }
        self.state.field_168 = true;
        self.state.docking_port_flag = false;
        self.state.docking_port_status = 0;
        let current = self.state.behavior;
        if current == -1 || current == ids::FREE_FLIGHT {
            return;
        }
        let Some((probe, dock_target)) = self
            .behavior(current)
            .map(|b| (b.docking_probe(), b.dock_target()))
        else {
            return;
        };
        self.state.field_168 = probe;
        if status == 0 || self.state.lock {
            self.state.docking_port_flag = true;
        }
        let mut proximity = false;
        let target = if current == ids::DOCK {
            dock_target
        } else if current == ids::FORMATION {
            proximity = true;
            // The leader's own dock target (single player) or target
            // (multiplayer) through the formation block's current leader: the
            // other manager's dock behaviour is not reachable here, so none.
            None
        } else {
            None
        };
        let Some(target) = target
            .and_then(|id| world.handle_of(id))
            .and_then(|h| world.inspect(h))
        else {
            return;
        };
        let mut in_range = true;
        if proximity && let Some(own) = world.inspect(ship) {
            let dx = e(target.position.x) - e(own.position.x);
            let dy = e(target.position.y) - e(own.position.y);
            let dz = e(target.position.z) - e(own.position.z);
            in_range = (dz * dz + dy * dy) + dx * dx < 1_000_000.0;
        }
        if in_range {
            self.state.docking_port_status = if target.type_flags & 0x80 != 0 { 2 } else { 1 };
        }
    }

    /// `update_current_behavior_direction(Dir)`: the player's steering,
    /// clamped, to the current behaviour (the level-camera frame is not
    /// ported: it needs `BuildPerpendicularFrame` with the camera, D193).
    pub fn update_current_behavior_direction(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        dir: Vector,
    ) {
        let mut clamped = dir;
        if f64::from(clamped.x) < -1.0 {
            clamped.x = -1.0;
        }
        if f64::from(clamped.x) > 1.0 {
            clamped.x = 1.0;
        }
        if f64::from(clamped.y) < -1.0 {
            clamped.y = -1.0;
        }
        if f64::from(clamped.y) > 1.0 {
            clamped.y = 1.0;
        }
        let zero = (e(clamped.z) * e(clamped.z) + e(clamped.y) * e(clamped.y))
            + e(clamped.x) * e(clamped.x)
            == 0.0;
        self.state.user_turning = !zero;
        let current = self.state.behavior;
        if Self::slot(current).is_some() {
            self.state.dirty = true;
            self.state.ship_up_flag = false;
            self.with(world, ship, current, |b, cx| b.set_direction(cx, clamped));
        }
        self.state.last_direction = clamped;
    }
}

/// `(x*x + y*y) + z*z`.
fn len_sq(v: Vector) -> f64 {
    (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)
}

/// The world's `rand()` as a sized [`AiRand`].
struct RandOf<'a, W: AiEnv + ?Sized>(&'a mut W);

impl<W: AiEnv + ?Sized> AiRand for RandOf<'_, W> {
    fn rand(&mut self) -> i32 {
        self.0.rand()
    }
}

/// A player input the manager forwards to the current behaviour.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PlayerInput {
    /// `update_current_behavior_afterburner`.
    Afterburner(bool),
    /// `update_current_behavior_brake_reverse`.
    BrakeReverse(bool),
    /// `update_current_behavior_engage_engine`.
    EngageEngine(bool),
    /// `update_current_behavior_slide_strafe_burst`.
    SlideStrafeBurst(StrafeDir),
    /// `update_current_behavior_throttle`.
    Throttle(f32),
}

/// `DirectiveOp::validate` for the ops that check anything (goto, face).
#[must_use]
pub fn validate(op: &DirectiveOp) -> bool {
    match op {
        DirectiveOp::Goto(g) => {
            if g.field_1c != 0
                && (g.destination.x != 0.0 || g.destination.y != 0.0 || g.destination.z != 0.0)
            {
                return false;
            }
            if g.range < 0.0 {
                return false;
            }
            if g.cruise != 0 && g.no_cruise != 0 {
                return false;
            }
            !(g.field_64 < g.field_60)
        }
        DirectiveOp::Face(f) => !(f.mode == 0 || (f.mode == 1 && f.target == 0)),
        _ => true,
    }
}
