//! The behaviour interface (`class Behavior`, `include/common_behavior.h`,
//! `Common/common_behavior.cpp`): what the behaviour manager drives, the
//! state every behaviour shares, the directive operations a behaviour is
//! handed, and the context a behaviour runs in.
//!
//! The reference's `Behavior` is a 27-slot vtable. [`Behavior`] keeps the
//! slots the manager and the player input call, by number in the method
//! docs; the slots with no recovered name keep the reconstruction's
//! descriptive names (`poll` for slot 5, `update` for slot 7). A behaviour
//! is generic over the world it runs in ([`AiEnv`]): the object world, or
//! the replay world over a recorded snapshot.
//!
//! Every behaviour embeds a [`BehaviorBase`], the `+0x04..+0x29` block the
//! base constructor writes.

use neuron_core::math::{Matrix, Vector};

use super::gun_convergence::LeaderNotification;
use super::manager::ManagerState;
use super::world::{AiRand, AiWorld, SpatialQueries, StrafeDir};

/// A world the behaviours and ideas can run in: inspection and commands,
/// the physics queries and `rand()`. The random source belongs to the
/// world because the reference has one C runtime generator that the AI
/// and the world's own code (gun spread) draw from in turn.
pub trait AiEnv: AiWorld + SpatialQueries + AiRand {}

impl<T: AiWorld + SpatialQueries + AiRand + ?Sized> AiEnv for T {}

/// The behaviour type ids (`BehaviorTypes`' registry, `CREATE_BEHAVIOR`).
pub mod ids {
    /// `NULL`.
    pub const NULL: i32 = 0;
    /// `Buzz`.
    pub const BUZZ: i32 = 1;
    /// `Goto`.
    pub const GOTO: i32 = 2;
    /// `Trail`.
    pub const TRAIL: i32 = 3;
    /// `Flee`.
    pub const FLEE: i32 = 4;
    /// `Evade`.
    pub const EVADE: i32 = 5;
    /// `Idle`.
    pub const IDLE: i32 = 6;
    /// `Dock`.
    pub const DOCK: i32 = 7;
    /// `Launch`.
    pub const LAUNCH: i32 = 8;
    /// `InstantTradeLane`.
    pub const INSTANT_TRADE_LANE: i32 = 9;
    /// `Formation`.
    pub const FORMATION: i32 = 10;
    /// `Large Ship (move)`.
    pub const LARGE_SHIP: i32 = 11;
    /// `Cruise`.
    pub const CRUISE: i32 = 12;
    /// `Strafe`.
    pub const STRAFE: i32 = 13;
    /// `Guide`.
    pub const GUIDE: i32 = 14;
    /// `Face`.
    pub const FACE: i32 = 15;
    /// `Loot`.
    pub const LOOT: i32 = 16;
    /// `Follow`.
    pub const FOLLOW: i32 = 17;
    /// `DrasticEvade`.
    pub const DRASTIC_EVADE: i32 = 18;
    /// `FreeFlight`.
    pub const FREE_FLIGHT: i32 = 19;
    /// `Delay`.
    pub const DELAY: i32 = 20;
}

/// `EvaluateResult`, the answer of slot 6 (`evaluate`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EvaluateResult {
    /// 0 `EvalOk`.
    Ok,
    /// 1 `EvalBlocked`.
    Blocked,
    /// 2 `EvalSkip`.
    Skip,
}

/// `CancelRequestType`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CancelRequest {
    /// 0.
    None,
    /// 1.
    Current,
    /// 2.
    All,
}

/// Behaviour's own block (`+0x04..+0x29`, written by its constructor
/// `ConstructBase`, its binder `BindContext` and slots 3, 4, 9, 10, 23).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BehaviorBase {
    /// +0x04 the type id.
    pub type_id: i32,
    /// +0x08 the owner's id (`BindContext`).
    pub owner_id: u32,
    /// +0x0c the last answer of slot 5 (2 after construction).
    pub mode: i32,
    /// +0x10 the last result of slot 7, read by slot 9 (1 after
    /// construction).
    pub result: i32,
    /// +0x14 the owner is bound (the reference's owner pointer is
    /// non-null).
    pub bound: bool,
    /// +0x1c the time the manager hands over before slot 7.
    pub tick_delta: f32,
    /// +0x28 the flag byte: bit 0 cleared by slot 3, bit 1 set by a
    /// directive (slot 10) and cleared by slot 23.
    pub flags: u8,
    /// +0x29 cleared by slot 4; slot 5 answers 3 while it is set.
    pub flag29: u8,
}

impl BehaviorBase {
    /// `Behavior::ConstructBase`: mode 2, result 1, everything else zero.
    #[must_use]
    pub fn new(type_id: i32) -> Self {
        Self {
            type_id,
            mode: 2,
            result: 1,
            ..Self::default()
        }
    }

    /// `BindContext(Owner, Manager, Sys)`: refused without an owner.
    pub fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let Some(id) = owner_id else {
            return false;
        };
        self.bound = true;
        self.owner_id = id;
        true
    }

    /// `Behavior::Slot3`: mode 1, result 1, flag bit 0 cleared, no tick
    /// delta.
    pub fn engage(&mut self) {
        self.mode = 1;
        self.result = 1;
        self.flags &= 0xfe;
        self.tick_delta = 0.0;
    }

    /// `Behavior::Slot4`: the pathfinder's published target cleared, flag
    /// +0x29 cleared.
    pub fn disengage(&mut self, state: &mut ManagerState) {
        state.pathfind.target_id = 0;
        self.flag29 = 0;
    }

    /// `Behavior::cancel_notify` (slot 23): flag bit 1 cleared.
    pub fn cancel_notify(&mut self) {
        self.flags &= 0xfd;
    }

    /// The slot-5 answer most simple behaviours give
    /// (`SetModeFromFlags`): 3 while +0x29 is set, else 1, kept in +0x0c.
    pub fn mode_from_flags(&mut self) -> i32 {
        self.mode = if self.flag29 != 0 { 3 } else { 1 };
        self.mode
    }
}

/// A behaviour's tick context: the world (with the random source), the
/// ship the manager drives and the manager's state (which holds the
/// system manager's parts the behaviours reach: the thrust manager, the
/// pathfinder state, the personality, the directive result).
pub struct Cx<'a, W: AiEnv + ?Sized> {
    /// The world.
    pub world: &'a mut W,
    /// The ship (the manager's watched object).
    pub ship: W::Handle,
    /// The manager's state.
    pub state: &'a mut ManagerState,
    /// The manager's behaviours (`BehaviorDispatchTable`), for a behaviour
    /// that drives another (the flee behaviour hands the ship to the goto,
    /// dock or formation behaviour); the entry of the behaviour being
    /// called holds a stand-in. `None` outside the manager.
    pub peers: Option<&'a mut [Box<dyn Behavior<W>>]>,
}

impl<'a, W: AiEnv + ?Sized> Cx<'a, W> {
    /// The context of a tick outside the manager's dispatch table.
    pub fn new(world: &'a mut W, ship: W::Handle, state: &'a mut ManagerState) -> Self {
        Self {
            world,
            ship,
            state,
            peers: None,
        }
    }

    /// Runs `f` on the manager's behaviour `index` with this tick's world
    /// and state; `None` outside the manager or for an index outside the
    /// table.
    pub fn with_peer<T>(
        &mut self,
        index: i32,
        f: impl FnOnce(&mut dyn Behavior<W>, &mut Cx<'_, W>) -> T,
    ) -> Option<T> {
        let peers = self.peers.as_deref_mut()?;
        let b = peers.get_mut(usize::try_from(index).ok()?)?;
        let mut cx = Cx {
            world: &mut *self.world,
            ship: self.ship,
            state: &mut *self.state,
            peers: None,
        };
        Some(f(&mut **b, &mut cx))
    }

    /// The content-callback report of `IBehaviorManager::NotifyFollowEvent`,
    /// `NotifyTetherEvent` and `GuideBehavior::NotifyManagerGuideEvent`:
    /// made only while the ship is in an attack state (`AttackState != 0`)
    /// and its current behaviour is one of `behaviors`, and only to an
    /// installed callback ([`super::world::AiWorld::content_event`]).
    /// True when it was made; without a callback the reference asks the
    /// ship `is_player()` for nothing.
    pub fn notify_content(
        &mut self,
        behaviors: &[i32],
        event: super::content::CombatEvent,
    ) -> bool {
        if self.state.sys.attack_state() == 0 || !behaviors.contains(&self.state.behavior) {
            return false;
        }
        let handle = self.state.content_handle;
        if handle == 0 {
            return false;
        }
        self.world.content_event(self.ship, handle, event);
        true
    }

    /// `rand()`.
    pub fn rand(&mut self) -> i32 {
        self.world.rand()
    }

    /// `SetDirectiveResult(Word11c, Word118)`: which behaviour finished
    /// and the result word the directive callbacks receive.
    pub fn set_directive_result(&mut self, behavior: i32, result: i32) {
        self.state.f118 = result;
        self.state.f11c = behavior;
    }

    /// `ThrustManager::SetThrottle` on the ship.
    pub fn set_throttle(&mut self, throttle: f32, mode: i32) -> bool {
        self.state
            .thrust
            .set_throttle(&mut *self.world, self.ship, throttle, mode)
    }

    /// `ThrustManager::SetThrustersActive` on the ship.
    pub fn set_thrusters_active(&mut self, active: bool) {
        self.state
            .thrust
            .set_thrusters_active(&mut *self.world, self.ship, active);
    }

    /// `ThrustManager::SetStrafeDir` on the ship.
    pub fn set_strafe_dir(&mut self, dir: StrafeDir) {
        self.state
            .thrust
            .set_strafe_dir(&mut *self.world, self.ship, dir);
    }

    /// `ThrustManager::engage_cruise` on the ship.
    pub fn engage_cruise(&mut self, engage: bool) {
        self.state
            .thrust
            .engage_cruise(&mut *self.world, self.ship, engage);
    }

    /// `ThrustManager::SetAxisThrottle` on the ship.
    pub fn set_axis_throttle(&mut self, wanted: Vector) {
        self.state
            .thrust
            .set_axis_throttle(&mut *self.world, self.ship, wanted);
    }

    /// `ThrustManager::SetNudgeDirection` on the ship.
    pub fn set_nudge_direction(&mut self, world_dir: Vector) {
        self.state
            .thrust
            .set_nudge_direction(&mut *self.world, self.ship, world_dir);
    }

    /// `ThrustManager::SetEngineActive` on the ship.
    pub fn set_engine_active(&mut self, active: bool) {
        self.state
            .thrust
            .set_engine_active(&mut *self.world, self.ship, active);
    }

    /// `ThrustManager::CheckSteeringGate` for the ship.
    #[must_use]
    pub fn check_steering_gate(&self) -> bool {
        let is_player = self.world.inspect(self.ship).is_some_and(|v| v.is_player);
        self.state.steering_gate(is_player)
    }

    /// `Behavior::SetCruiseLock(Value, Mask)`: the pathfinder's.
    pub fn set_cruise_lock(&mut self, value: u8, mask: u32) {
        self.state.pathfind.set_cruise_lock(
            &mut self.state.thrust,
            &mut *self.world,
            self.ship,
            value,
            mask,
        );
    }

    /// The ship's view.
    #[must_use]
    pub fn own(&self) -> Option<super::world::ObjectView> {
        self.world.inspect(self.ship)
    }

    /// An object's view by id.
    #[must_use]
    pub fn view_of(&self, id: Option<u32>) -> Option<super::world::ObjectView> {
        let id = id?;
        self.world.handle_of(id).and_then(|h| self.world.inspect(h))
    }

    /// `IObjAffect::slot_5c(Kind, &Target, &Second, Flag, this)` from the
    /// behaviour `index` (`Ship::slot_5c`): refused while another request
    /// is in progress; otherwise the behaviour becomes the ship's pending
    /// callback. The answers the server gave during the call follow
    /// ([`super::world::AiWorld::take_request_answers`]); the caller hands
    /// them to its `OnResult` and ends the request when it does not keep
    /// it ([`Cx::end_request`]).
    pub fn request(
        &mut self,
        index: i32,
        kind: i32,
        target: u32,
        second: u32,
        flag: i32,
    ) -> Vec<super::world::RequestAnswer> {
        self.world.command(
            self.ship,
            super::world::AiCommand::Request {
                kind,
                target,
                second,
                flag,
            },
        );
        if self.state.request_busy {
            return Vec::new();
        }
        self.state.pending_request = Some(index);
        self.state.request_busy = true;
        self.world.take_request_answers(self.ship)
    }

    /// The request ends: the ship's pending callback and its busy flag
    /// cleared (`Ship::request_returned` after an answer `OnResult` does
    /// not keep, and `Ship::slot_60`).
    pub fn end_request(&mut self) {
        self.state.pending_request = None;
        self.state.request_busy = false;
    }

    /// `IObjAffect::slot_60(Kind, &Target, Arg)` (`Ship::slot_60`): the
    /// pending callback dropped, then the server told.
    pub fn cancel_request(&mut self, kind: i32, target: u32, arg: i32) {
        self.end_request();
        self.world.command(
            self.ship,
            super::world::AiCommand::CancelRequest { kind, target, arg },
        );
    }

    /// The idle stop (`IdleIdea::Update`).
    pub fn stop(&mut self) {
        self.state.thrust.stop(&mut *self.world, self.ship);
    }

    /// `ThrustManager::BlendNudgeDirection` on the ship.
    pub fn blend_nudge_direction(&mut self, world_dir: Vector, dt: f32) {
        self.state
            .thrust
            .blend_nudge_direction(&mut *self.world, self.ship, world_dir, dt);
    }

    /// `TargetManager::GetTarget`: the ship's target (`get_target`), when
    /// the world has it.
    #[must_use]
    pub fn target_view(&self) -> Option<super::world::ObjectView> {
        self.view_of(self.own().and_then(|v| v.target))
    }

    /// Whether timer `slot` of the system manager's timer table is latched.
    #[must_use]
    pub fn timer_active(&self, slot: usize) -> bool {
        self.state.sys.timers.active(slot)
    }

    /// The ship's live formation leader (`get_formation_leader`).
    fn formation_leader(&self) -> Option<u32> {
        let id = self.own().and_then(|v| v.formation_leader)?;
        self.world.handle_of(id).map(|_| id)
    }

    /// `FGunConvergenceStateBlock::NotifyFormationLeader1(Delay)`: break
    /// apart now (1) and, after a positive delay, regroup (2).
    pub fn notify_formation_leader_1(&mut self, delay: f32) {
        let Some(leader) = self.formation_leader() else {
            return;
        };
        self.state.leader_notes.push(LeaderNotification {
            leader,
            extra: 1,
            duration: 0.0,
        });
        if delay > 0.0 {
            self.state.leader_notes.push(LeaderNotification {
                leader,
                extra: 2,
                duration: delay,
            });
        }
    }

    /// `FGunConvergenceStateBlock::NotifyFormationLeader2`: regroup now.
    pub fn notify_formation_leader_2(&mut self) {
        let Some(leader) = self.formation_leader() else {
            return;
        };
        self.state.leader_notes.push(LeaderNotification {
            leader,
            extra: 2,
            duration: 0.0,
        });
    }
}

/// `pub::AI::DirectiveGotoOp` (0x70 bytes), field by field.
#[derive(Clone, Debug, PartialEq)]
pub struct GotoOp {
    /// +0x0c the target kind (3 by default; 1 a point).
    pub kind: i32,
    /// +0x10 the destination.
    pub destination: Vector,
    /// +0x1c gates the NaN check of `validate`.
    pub field_1c: i32,
    /// +0x20..+0x4c twelve words (object ids and spline data).
    pub words_20: [u32; 12],
    /// +0x50 the arrival range (500).
    pub range: f32,
    /// +0x54 (-1).
    pub field_54: f32,
    /// +0x58 (0).
    pub field_58: u8,
    /// +0x59 (1).
    pub field_59: u8,
    /// +0x5a cruise (0).
    pub cruise: u8,
    /// +0x5b no cruise (0).
    pub no_cruise: u8,
    /// +0x5c (0).
    pub field_5c: u32,
    /// +0x60 (200).
    pub field_60: f32,
    /// +0x64 (500).
    pub field_64: f32,
    /// +0x68 (0).
    pub field_68: u32,
    /// +0x6c (0).
    pub field_6c: u8,
}

impl Default for GotoOp {
    /// `DirectiveGotoOp::DirectiveGotoOp`.
    fn default() -> Self {
        Self {
            kind: 3,
            destination: Vector::ZERO,
            field_1c: 0,
            words_20: [0; 12],
            range: 500.0,
            field_54: -1.0,
            field_58: 0,
            field_59: 1,
            cruise: 0,
            no_cruise: 0,
            field_5c: 0,
            field_60: 200.0,
            field_64: 500.0,
            field_68: 0,
            field_6c: 0,
        }
    }
}

/// `pub::AI::DirectiveFaceOp` (0x48 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FaceOp {
    /// +0x0c 1 an object, 2 a point, 3 an orientation, 4 no target.
    pub mode: i32,
    /// +0x10 the object of mode 1.
    pub target: u32,
    /// +0x14 the point of mode 2.
    pub point: Vector,
    /// +0x20 the orientation of mode 3.
    pub orientation: Matrix,
    /// +0x44 (1).
    pub field_44: u8,
}

impl Default for FaceOp {
    /// `DirectiveFaceOp::DirectiveFaceOp`: the orientation all zeros.
    fn default() -> Self {
        Self {
            mode: 0,
            target: 0,
            point: Vector::ZERO,
            orientation: Matrix::from_rows([[0.0; 3]; 3]),
            field_44: 1,
        }
    }
}

/// `pub::AI::DirectiveDrasticEvadeOp` (0x30 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct DrasticEvadeOp {
    /// +0x0c the threat id, which `DrasticEvadeBehavior` reads as its
    /// kind (0 nudge, 1 reverse).
    pub threat: u32,
    /// +0x10..+0x18 three words; the nudge kind reads the first two as
    /// floats.
    pub words_10: [u32; 3],
    /// +0x1c the duration (1).
    pub duration: f32,
    /// +0x20..+0x2c.
    pub words_20: [u32; 4],
}

impl Default for DrasticEvadeOp {
    fn default() -> Self {
        Self {
            threat: 0,
            words_10: [0; 3],
            duration: 1.0,
            words_20: [0; 4],
        }
    }
}

/// `pub::AI::DirectiveDockOp` (0x2c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct DockOp {
    /// +0x0c the object to dock with.
    pub dock_object: u32,
    /// +0x10 the trade lane's other ring (an object id; 0 none).
    pub field_10: u32,
    /// +0x14 the port is already granted.
    pub field_14: u8,
    /// +0x18 the dock port (-1).
    pub port: i32,
    /// +0x1c the object the proximity watcher watches (0 none).
    pub field_1c: u32,
    /// +0x20 its near threshold (200).
    pub field_20: f32,
    /// +0x24 its far threshold (500).
    pub field_24: f32,
    /// +0x28 its cruise ratio, a float's bits (0: from the speeds).
    pub field_28: u32,
}

impl Default for DockOp {
    /// `DirectiveDockOp::DirectiveDockOp`.
    fn default() -> Self {
        Self {
            dock_object: 0,
            field_10: 0,
            field_14: 0,
            port: -1,
            field_1c: 0,
            field_20: 200.0,
            field_24: 500.0,
            field_28: 0,
        }
    }
}

/// `pub::AI::DirectiveFollowOp` (0x24 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct FollowOp {
    /// +0x0c the object to follow (`validate`: not 0).
    pub target: u32,
    /// +0x10 the follow distance (150).
    pub distance: f32,
    /// +0x14..+0x1c the offset in the leader's frame.
    pub offset: Vector,
    /// +0x20 the range the follow behaviour's poll uses (400).
    pub range: f32,
}

impl Default for FollowOp {
    /// `DirectiveFollowOp::DirectiveFollowOp`.
    fn default() -> Self {
        Self {
            target: 0,
            distance: 150.0,
            offset: Vector::ZERO,
            range: 400.0,
        }
    }
}

/// `pub::AI::DirectiveLaunchOp` (0x18 bytes).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LaunchOp {
    /// +0x0c the object to launch from.
    pub launch_object: u32,
    /// +0x10 the docking sphere (-1).
    pub index: i32,
    /// +0x14 its terminal type (0).
    pub terminal: i32,
}

impl Default for LaunchOp {
    /// `DirectiveLaunchOp::DirectiveLaunchOp`.
    fn default() -> Self {
        Self {
            launch_object: 0,
            index: -1,
            terminal: 0,
        }
    }
}

/// `pub::AI::DirectiveGuideOp` (0xbc bytes): a goto op, a follow op and
/// the guide's own parameters.
#[derive(Clone, Debug, PartialEq)]
pub struct GuideOp {
    /// +0x0c the goto part (its `field_5c` is the guided ship).
    pub goto: GotoOp,
    /// +0x7c the follow part.
    pub follow: FollowOp,
    /// +0xa0 start by following (1).
    pub field_a0: u8,
    /// +0xa4 the sampling period (10).
    pub field_a4: f32,
    /// +0xa8 the course angle (π/8).
    pub field_a8: f32,
    /// +0xac (3).
    pub field_ac: i32,
    /// +0xb0 the closing threshold (10).
    pub field_b0: f32,
    /// +0xb4 the falling-behind threshold (-10).
    pub field_b4: f32,
    /// +0xb8 the poll's range (400).
    pub field_b8: f32,
}

impl Default for GuideOp {
    /// `DirectiveGuideOp::DirectiveGuideOp`.
    fn default() -> Self {
        Self {
            goto: GotoOp::default(),
            follow: FollowOp::default(),
            field_a0: 1,
            field_a4: 10.0,
            field_a8: f32::from_bits(0x3ec9_0fdb),
            field_ac: 3,
            field_b0: 10.0,
            field_b4: -10.0,
            field_b8: 400.0,
        }
    }
}

/// `pub::AI::DirectiveInstantTradelaneOp` (0x18 bytes).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InstantTradeLaneOp {
    /// +0x0c the entrance ring.
    pub entrance: u32,
    /// +0x10 the exit ring (0 none).
    pub exit: u32,
    /// +0x14 the speed (a float in the third word).
    pub speed: f32,
}

/// `pub::AI::DirectiveTrailOp` (0x1c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct TrailOp {
    /// +0x0c the object to trail (`validate`: not 0).
    pub target: u32,
    /// +0x10 the trail distance (150).
    pub distance: f32,
    /// +0x14 (0).
    pub field_14: f32,
    /// +0x18 (0).
    pub field_18: f32,
}

impl Default for TrailOp {
    /// `DirectiveTrailOp::DirectiveTrailOp`.
    fn default() -> Self {
        Self {
            target: 0,
            distance: 150.0,
            field_14: 0.0,
            field_18: 0.0,
        }
    }
}

/// `pub::AI::DirectiveTractorBeamOp` (0x14 bytes).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TractorBeamOp {
    /// +0x0c the object to tractor in (`validate`: not 0).
    pub target: u32,
    /// +0x10 (1), which `LootBehavior` keeps when the object is gone.
    pub field_10: u8,
}

impl Default for TractorBeamOp {
    /// `DirectiveTractorBeamOp::DirectiveTractorBeamOp`.
    fn default() -> Self {
        Self {
            target: 0,
            field_10: 1,
        }
    }
}

/// A directive operation (`pub::AI::BaseOp`), the argument of slot 10.
/// Kinds without a ported reader keep their op type only.
#[derive(Clone, Debug, PartialEq)]
pub enum DirectiveOp {
    /// `DirectiveGotoOp` (op type 3).
    Goto(GotoOp),
    /// `DirectiveFollowOp` (4).
    Follow(FollowOp),
    /// `DirectiveTrailOp` (5).
    Trail(TrailOp),
    /// `DirectiveFaceOp` (7).
    Face(FaceOp),
    /// `DirectiveTractorBeamOp` (9).
    TractorBeam(TractorBeamOp),
    /// `DirectiveEvadeOp` (10): the threat to evade.
    Evade(u32),
    /// `DirectiveDrasticEvadeOp` (11).
    DrasticEvade(DrasticEvadeOp),
    /// `DirectiveDockOp` (13).
    Dock(DockOp),
    /// `DirectiveLaunchOp` (14).
    Launch(LaunchOp),
    /// `DirectiveInstantTradelaneOp` (15).
    InstantTradeLane(InstantTradeLaneOp),
    /// `DirectiveDelayOp` (12): the delay in seconds.
    Delay(f32),
    /// `DirectiveGuideOp` (17).
    Guide(Box<GuideOp>),
    /// `DirectiveIdleOp` (20).
    Idle,
    /// `DirectiveCancelOp` (21).
    Cancel,
    /// Any other op, by its `OpType`.
    Other(i32),
}

impl DirectiveOp {
    /// The `OpType`.
    #[must_use]
    pub fn op_type(&self) -> i32 {
        match self {
            Self::Goto(_) => 3,
            Self::Follow(_) => 4,
            Self::Trail(_) => 5,
            Self::Face(_) => 7,
            Self::TractorBeam(_) => 9,
            Self::Evade(_) => 10,
            Self::DrasticEvade(_) => 11,
            Self::Dock(_) => 13,
            Self::Launch(_) => 14,
            Self::InstantTradeLane(_) => 15,
            Self::Delay(_) => 12,
            Self::Guide(_) => 17,
            Self::Idle => 20,
            Self::Cancel => 21,
            Self::Other(t) => *t,
        }
    }

    /// The behaviour `pub::AI::SubmitDirective` runs for the op
    /// (`GSubmitDirectiveBehaviors`), `None` for an op it refuses.
    #[must_use]
    pub fn behavior(&self) -> Option<i32> {
        Some(match self.op_type() {
            3 => ids::GOTO,
            4 => ids::FOLLOW,
            5 => ids::TRAIL,
            13 => ids::DOCK,
            12 => ids::DELAY,
            20 => ids::IDLE,
            14 => ids::LAUNCH,
            15 => ids::INSTANT_TRADE_LANE,
            17 => ids::GUIDE,
            7 => ids::FACE,
            10 => ids::EVADE,
            11 => ids::DRASTIC_EVADE,
            9 => ids::LOOT,
            _ => return None,
        })
    }
}

/// The payload of the manager's type-4 event (`FBehaviorTradelaneEvent`,
/// `include/common_iface.h`): the server's `notify_go_tradelane` (a ship
/// entered a lane) and `notify_stop_tradelane` (it left), and
/// `CShip::request_exit_tradelane` to a leader's followers.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LaneEvent {
    /// +0x00 1 entering a lane, 0 leaving it.
    pub entering: bool,
    /// +0x01 the leader asked its followers out of the lane.
    pub exit_requested: bool,
    /// +0x02 `bFastExit`.
    pub fast_exit: bool,
    /// +0x04 the lane speed `go_tradelane` was given.
    pub speed: f32,
    /// +0x0c the ship.
    pub ship: u32,
    /// +0x10 the entrance ring.
    pub entrance: u32,
    /// +0x14 the exit ring (0 when leaving).
    pub exit: u32,
}

/// One behaviour (`class Behavior`). Slot numbers of the reference's
/// vtable in brackets; the defaults are `Behavior`'s own bodies.
pub trait Behavior<W: AiEnv + ?Sized> {
    /// The shared block.
    fn base(&self) -> &BehaviorBase;

    /// The shared block, mutably.
    fn base_mut(&mut self) -> &mut BehaviorBase;

    /// Whether the port has this behaviour; the replay skips the ticks of
    /// the others.
    fn ported(&self) -> bool {
        true
    }

    /// The concrete behaviour, for a harness that sets its fields from a
    /// record. The default offers none.
    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        None
    }

    /// The concrete behaviour, for a reader of its fields (another ship's
    /// loot behaviour reads this one's target). The default offers none.
    fn as_any(&self) -> Option<&dyn core::any::Any> {
        None
    }

    /// (1) `SetContext(Owner, Manager, Sys)`: binds the owner.
    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        self.base_mut().bind(owner_id)
    }

    /// The world-dependent half of slot 1 (`SetContext`): the ideas'
    /// `SetContext` calls, which read the ship's motion model. The manager
    /// calls it right after [`Behavior::bind`] when it is built against a
    /// world.
    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        let _ = cx;
    }

    /// (3) becomes the current behaviour.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        let _ = cx;
        self.base_mut().engage();
    }

    /// (4) stops being the current behaviour.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.base_mut().disengage(cx.state);
    }

    /// (5) the manager's poll: 0 "certainly me", 1 "keep the weight", 2
    /// to 4 "certainly not" (4 also ends the current behaviour). The
    /// argument is the manager's `BehaviorTimerC`.
    fn poll(&mut self, cx: &mut Cx<'_, W>, timer_c: f32) -> i32;

    /// (6) `evaluate`, for the player's manoeuvres.
    fn evaluate(&mut self, cx: &mut Cx<'_, W>, context: f32) -> EvaluateResult {
        let _ = (cx, context);
        EvaluateResult::Skip
    }

    /// (7) the behaviour's tick; its result is kept in
    /// [`BehaviorBase::result`].
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32;

    /// (8) the end-of-tick call.
    fn end_tick(&mut self, cx: &mut Cx<'_, W>) {
        let _ = cx;
    }

    /// (9) the status the manager reads: the last result of slot 7.
    fn status(&self) -> i32 {
        self.base().result
    }

    /// (10) the directive the behaviour is started with.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let _ = (cx, op);
        true
    }

    /// (11) the player's docking probe flag.
    fn docking_probe(&self) -> bool {
        false
    }

    /// (12) the player's steering direction.
    fn set_direction(&mut self, cx: &mut Cx<'_, W>, dir: Vector) {
        let _ = (cx, dir);
    }

    /// (13) the player's throttle.
    fn set_throttle(&mut self, cx: &mut Cx<'_, W>, throttle: f32) {
        let _ = (cx, throttle);
    }

    /// (14) the player's cruise toggle.
    fn set_cruise(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        let _ = (cx, on);
    }

    /// (15) the player's engine toggle.
    fn engage_engine(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        let _ = (cx, on);
    }

    /// (16) `set_afterburner`: nothing while cruising outside a lane;
    /// otherwise the thrusters, and with them full throttle (mode 2 in a
    /// lane).
    fn set_afterburner(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        let (cruise, lane) = cx
            .world
            .inspect(cx.ship)
            .and_then(|v| v.cruise)
            .unwrap_or((false, false));
        if cruise && !lane {
            return;
        }
        if !on {
            cx.set_thrusters_active(false);
            return;
        }
        cx.set_thrusters_active(true);
        cx.set_throttle(1.0, if lane { 2 } else { 0 });
    }

    /// (17) `slide_strafe_burst`.
    fn slide_strafe_burst(&mut self, cx: &mut Cx<'_, W>, dir: StrafeDir) {
        cx.set_strafe_dir(dir);
    }

    /// (18) the player's brake.
    fn brake_reverse(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        let _ = (cx, on);
    }

    /// (19) `allow_head_tracking`.
    fn allow_head_tracking(&self) -> bool {
        true
    }

    /// (20) `cancel_behavior`.
    fn cancel_behavior(&mut self, cx: &mut Cx<'_, W>, request: CancelRequest) -> bool {
        let _ = (cx, request);
        true
    }

    /// (21) an object left the system.
    fn object_left(&mut self, cx: &mut Cx<'_, W>, id: u32) {
        let _ = (cx, id);
    }

    /// (22) the manager's type-4 event (`IBehaviorManager::Slot00` case 4
    /// hands it to the current behaviour's `Slot22`): a ship entered or
    /// left a trade lane, or a leader asked its followers out. `Behavior`'s
    /// own slot does nothing.
    fn lane_event(&mut self, cx: &mut Cx<'_, W>, event: &LaneEvent) {
        let _ = (cx, event);
    }

    /// (23) `cancel_notify`: the directive flag cleared.
    fn cancel_notify(&mut self) {
        self.base_mut().cancel_notify();
    }

    /// (24) `get_target`.
    fn target(&self) -> Option<u32> {
        None
    }

    /// (26) the dock behaviour's target, for the player's docking probe.
    fn dock_target(&self) -> Option<u32> {
        None
    }

    /// `IObjAffect::RequestCB::OnResult(RequestType, Response, Arg3)`: the
    /// answer to a request the behaviour made (the dock and trade-lane
    /// behaviours); true keeps the request pending. The default lets go.
    fn on_request_result(
        &mut self,
        cx: &mut Cx<'_, W>,
        kind: i32,
        response: i32,
        arg: i32,
    ) -> bool {
        let _ = (cx, kind, response, arg);
        false
    }
}
