//! `pub::AI` (`Server/server_pub.cpp:6946-7541`): Content's hold on a
//! ship's behaviour manager ([`game_sim::ai::manager::BehaviorManager`],
//! the world's per-object manager, made on first use as
//! `GetBehaviorManager` makes it), and the directives a player's client
//! is told instead.
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `SubmitState` | 6991 | [`submit_state`] |
//! | `SubmitDirective` | 7137 | [`submit_directive`] |
//! | `set_directive_priority` | 7280 | [`set_directive_priority`] |
//! | `get_behavior_id` | 7290 | [`behavior_id`] |
//! | `get_state_graph_id` | 7301 | [`state_graph_id`] |
//! | `get_personality` | 7318 | [`personality`] |
//! | `enable_maneuver` | 7330 | [`enable_maneuver`] |
//! | `enable_all_maneuvers` | 7342 | [`enable_all_maneuvers`] |
//! | `lock_maneuvers` | 7352 | [`lock_maneuvers`] |
//! | `get_scan_response` | 7372 | [`scan_response`] |
//! | `submit_forced_target` | 7394 | [`submit_forced_target`] |
//! | `remove_forced_target` | 7407 | [`remove_forced_target`] |
//! | `set_player_enemy_clamp` | 7421 | [`set_player_enemy_clamp`] |
//! | `update_formation_state` | 7475 | [`update_formation_state`] |
//! | `refresh_state_graph` | 7533 | [`refresh_state_graph`] |
//!
//! - **The ops.** Content's `pub::AI::BaseOp` family is [`BaseOp`]: the
//!   priority and one [`Op`]. The directives game-sim's behaviours read
//!   are its [`DirectiveOp`]s; the three `Set*Params` of `SubmitState`
//!   and the directives no behaviour of `SubmitDirective` takes (strafe,
//!   ram, formation, wait for a ship, wait for a player's manoeuvre) are
//!   kept here with the fields their `validate` reads. [`validate`] is
//!   every op's `validate`.
//! - **Result codes.** `SubmitState`, `SubmitDirective` and
//!   `set_directive_priority` answer an [`OpRType`] (0 to 5, the C++'s
//!   literals); `update_formation_state` -3 as [`PubError::NOT_FOUND`];
//!   the others their own value, false, 0 or -1 for a missing object or
//!   manager.
//! - **A player's ship.** `SubmitDirective` on the ship a live player
//!   flies does not touch its manager: the six kinds a client takes
//!   (cancel, delay, dock, follow, idle, goto) go to the owner as
//!   `SETDIRECTIVE`; `get_scan_response` tells the owner of the scan
//!   (`SCANNOTIFY`) and answers 3.
//! - **Callbacks** are the owner's handles (D194, D710; record 0008 point
//!   4): a content callback by its handle with the creation type Content's
//!   object answers, a directive callback by its handle in the manager's
//!   five slots.

use game_sim::ai::behavior::DirectiveOp;
use game_sim::ai::manager::BehaviorManager;
use game_sim::ai::pathfind::UserZone;
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::behavior_id as behavior_type_id;
use game_sim::object::{ObjHandle, ObjectKind, World};
use game_sim::protocol::to_client::Directive;
use neuron_core::math::{Matrix, Vector};

use super::PubError;
use crate::client;
use crate::dock::lookup_live;
use crate::server::Cx;

/// `pub::AI::OP_RTYPE`, the literal the C++ returns (it names none).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OpRType(pub i32);

impl OpRType {
    /// 0: `SubmitState` applied the personality params or cleared the zone.
    pub const SUCCESS: Self = Self(0);
    /// 1: `SubmitDirective` accepted the directive (sent to the player's
    /// client or set on the AI); `set_directive_priority` stored it.
    pub const ACCEPTED: Self = Self(1);
    /// 2: the AI already has a current directive.
    pub const DIRECTIVE_BUSY: Self = Self(2);
    /// 3: `set_directive_priority` without a current directive.
    pub const NO_DIRECTIVE: Self = Self(3);
    /// 4: a zero id in both, and every other exit of `SubmitState` (a zone,
    /// a flag, an unknown op, a missing object).
    pub const FOUR: Self = Self(4);
    /// 5: an op that fails `validate`, a missing object, or an op with no
    /// behaviour.
    pub const INVALID: Self = Self(5);
}

/// A callback field of `SetPersonalityParams`: `(T*)-1` keeps the
/// manager's, null clears it, or an object.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CallbackArg {
    /// `-1`: the manager's is left as it is (the default).
    #[default]
    Keep,
    /// Null.
    Null,
    /// A callback by the owner's handle of it (not 0, not `u32::MAX`;
    /// record 0008 point 4): the manager asks a content callback's
    /// creation type of Content when it is installed.
    Set(u32),
}

/// `pub::AI::SetPersonalityParams` (op type 0).
#[derive(Clone, Debug, PartialEq)]
pub struct SetPersonalityParams {
    /// +0x0c the state graph (`validate`: not -1).
    pub state_graph: i32,
    /// +0x10 the content callback.
    pub content_callback: CallbackArg,
    /// +0x14 the directive callback.
    pub directive_callback: CallbackArg,
    /// +0x18 the personality is set.
    pub set_personality: bool,
    /// +0x1c the personality.
    pub personality: Personality,
}

impl Default for SetPersonalityParams {
    /// `SetPersonalityParams::SetPersonalityParams`.
    fn default() -> Self {
        Self {
            state_graph: -1,
            content_callback: CallbackArg::Keep,
            directive_callback: CallbackArg::Keep,
            set_personality: false,
            personality: Personality::default(),
        }
    }
}

/// `pub::AI::SetZoneBehaviorParams` (op type 1): the pathfinder's user
/// zone.
#[derive(Clone, Debug, PartialEq)]
pub struct SetZoneBehaviorParams {
    /// +0x0c 0 a sphere (the point and radius), 1 a box (the corners), 2 a
    /// live object's sphere (the object and radius).
    pub mode: i32,
    /// +0x10 0 normal, 1 inverted, 2 clear the zone (2).
    pub invert_mode: i32,
    /// +0x14 the sphere's centre.
    pub target_point: Vector,
    /// +0x20 the object of mode 2.
    pub target_id: u32,
    /// +0x24 the box's first corner.
    pub box_min: Vector,
    /// +0x30 the box's other corner.
    pub box_max: Vector,
    /// +0x3c the radius.
    pub radius: f32,
}

impl Default for SetZoneBehaviorParams {
    /// `SetZoneBehaviorParams::SetZoneBehaviorParams` (the mode and the
    /// box zeroed, as the C++ port does).
    fn default() -> Self {
        Self {
            mode: 0,
            invert_mode: 2,
            target_point: Vector::ZERO,
            target_id: 0,
            box_min: Vector::ZERO,
            box_max: Vector::ZERO,
            radius: 0.0,
        }
    }
}

/// `pub::AI::SetFlagParams` (op type 2).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SetFlagParams {
    /// +0x0c 0 the pathfinder's `Flag245`, 1 the gun groups' fire control
    /// (2; `validate`: not 2).
    pub flag_id: i32,
    /// +0x10 the value.
    pub value: u8,
}

impl Default for SetFlagParams {
    /// `SetFlagParams::SetFlagParams`.
    fn default() -> Self {
        Self {
            flag_id: 2,
            value: 0,
        }
    }
}

/// The derived part of a `pub::AI::BaseOp`.
#[derive(Clone, Debug, PartialEq)]
pub enum Op {
    /// `SetPersonalityParams` (0).
    SetPersonalityParams(Box<SetPersonalityParams>),
    /// `SetZoneBehaviorParams` (1).
    SetZoneBehaviorParams(SetZoneBehaviorParams),
    /// `SetFlagParams` (2).
    SetFlagParams(SetFlagParams),
    /// A directive game-sim's behaviours read (goto 3, follow 4, trail 5,
    /// face 7, tractor beam 9, evade 10, drastic evade 11, delay 12, dock
    /// 13, launch 14, instant trade lane 15, guide 17, idle 20, cancel 21).
    Directive(DirectiveOp),
    /// `DirectiveStrafeOp` (6): the object to strafe.
    Strafe(u32),
    /// `DirectiveRamOp` (8): the object to ram.
    Ram(u32),
    /// `DirectiveFormationOp` (16).
    Formation,
    /// `DirectiveWaitForShip` (18): the ship to wait for.
    WaitForShip(u32),
    /// `DirectiveWaitForPlayerManeuverOp` (19): the behaviour's name.
    WaitForPlayerManeuver(Vec<u8>),
}

impl Op {
    /// The `OpType`.
    #[must_use]
    pub fn op_type(&self) -> i32 {
        match self {
            Self::SetPersonalityParams(_) => 0,
            Self::SetZoneBehaviorParams(_) => 1,
            Self::SetFlagParams(_) => 2,
            Self::Directive(d) => d.op_type(),
            Self::Strafe(_) => 6,
            Self::Ram(_) => 8,
            Self::Formation => 16,
            Self::WaitForShip(_) => 18,
            Self::WaitForPlayerManeuver(_) => 19,
        }
    }
}

/// `pub::AI::BaseOp`: the op and its `Priority` (+0x08, a
/// `DirectivePriority`; 0 by default).
#[derive(Clone, Debug, PartialEq)]
pub struct BaseOp {
    /// +0x08 the priority, word 1 of the directive.
    pub priority: u32,
    /// The derived op.
    pub op: Op,
}

impl BaseOp {
    /// The op at priority 0.
    #[must_use]
    pub fn new(op: Op) -> Self {
        Self { priority: 0, op }
    }

    /// A directive at priority 0.
    #[must_use]
    pub fn directive(op: DirectiveOp) -> Self {
        Self::new(Op::Directive(op))
    }
}

/// `BaseOp::validate`, each op's own (`Common/common_pub.cpp:533-1050`).
#[must_use]
pub fn validate(op: &BaseOp) -> bool {
    match &op.op {
        Op::SetPersonalityParams(p) => p.state_graph != -1,
        Op::SetZoneBehaviorParams(z) => {
            if z.invert_mode != 2 {
                if z.radius == 0.0 {
                    return false;
                }
                if z.target_id == 0 {
                    let p = z.target_point;
                    let (x, y, zz) = (f64::from(p.x), f64::from(p.y), f64::from(p.z));
                    return (x * x + y * y) + zz * zz > 0.0;
                }
            }
            true
        }
        Op::SetFlagParams(f) => f.flag_id != 2,
        Op::Directive(d) => match d {
            DirectiveOp::Goto(_)
            | DirectiveOp::Face(_)
            | DirectiveOp::Follow(_)
            | DirectiveOp::Guide(_) => game_sim::ai::manager::validate(d),
            DirectiveOp::Trail(t) => t.target != 0,
            DirectiveOp::TractorBeam(t) => t.target != 0,
            DirectiveOp::Evade(threat) => *threat != 0,
            DirectiveOp::Dock(k) => {
                !(k.dock_object == 0
                    || (k.field_14 != 0 && k.port == -1)
                    || k.field_24 < k.field_20)
            }
            DirectiveOp::Launch(l) => l.launch_object != 0 && l.index != -1 && l.terminal != 0,
            DirectiveOp::InstantTradeLane(t) => t.entrance != 0,
            DirectiveOp::Delay(seconds) => *seconds != 0.0,
            DirectiveOp::DrasticEvade(_)
            | DirectiveOp::Idle
            | DirectiveOp::Cancel
            | DirectiveOp::Other(_) => true,
        },
        Op::Strafe(target) | Op::Ram(target) | Op::WaitForShip(target) => *target != 0,
        Op::Formation => true,
        Op::WaitForPlayerManeuver(name) => {
            let id = behavior_type_id(name);
            id < 0x15 && id != -1
        }
    }
}

/// `GetBehaviorManager(Obj)` and a call on it: `None` for an object that
/// cannot have a manager.
fn with_manager<T>(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    f: impl FnOnce(&mut BehaviorManager<World>, &mut World) -> T,
) -> Option<T> {
    cx.world.with_behavior_manager(h, f)
}

/// The live object of `id` and a call on its manager; `None` when either
/// is missing.
fn on_manager<T>(
    cx: &mut Cx<'_>,
    id: u32,
    f: impl FnOnce(&mut BehaviorManager<World>, &mut World, ObjHandle) -> T,
) -> Option<T> {
    let h = lookup_live(cx, id)?;
    with_manager(cx, h, |m, w| f(m, w, h))
}

/// `PlayerDB::is_valid_ship_owner(player, ship)`: a live player slot whose
/// ship is `ship`.
fn is_valid_ship_owner(cx: &Cx<'_>, player: u32, ship: u32) -> bool {
    cx.logic.players.is_valid(player)
        && cx
            .logic
            .players
            .get(player)
            .is_some_and(|p| p.ship_object_id == ship)
}

/// `SubmitState(Flid, Op)`: the state ops on the object's manager.
///
/// - `SetPersonalityParams`: the state graph (`set_state_graph(id,
///   false)`), the callbacks that are not `-1`, the personality when asked;
///   0.
/// - `SetZoneBehaviorParams`: invert mode 2 clears the pathfinder's user
///   zone (0); otherwise the zone (a sphere, the box around the two corners
///   with its centre and half extents rounded to `float`, or a live
///   object's sphere; an unknown mode submits the cleared zone) is
///   submitted; 4.
/// - `SetFlagParams`: flag 0 the pathfinder's `Flag245`, 1 the gun groups'
///   fire control, any other nothing; 4.
///
/// The op is not validated. 4 for a zero id, a missing object, any other
/// op, and an object without a manager (D711).
pub fn submit_state(cx: &mut Cx<'_>, flid: u32, op: &BaseOp) -> OpRType {
    if flid & 0x3fff_ffff == 0 {
        return OpRType::FOUR;
    }
    let Some(h) = lookup_live(cx, flid) else {
        return OpRType::FOUR;
    };
    let data = cx.world.data_arc();
    // `set_content_callback` asks the callback its creation type (record
    // 0008 point 4): Content's object answers by its handle.
    let creation_type = match &op.op {
        Op::SetPersonalityParams(p) => match p.content_callback {
            CallbackArg::Set(handle) => crate::content::callbacks::creation_type(cx, handle),
            CallbackArg::Keep | CallbackArg::Null => 3,
        },
        _ => 3,
    };
    with_manager(cx, h, |m, w| match &op.op {
        Op::SetPersonalityParams(p) => {
            m.set_state_graph(w, h, &data.state_graphs, p.state_graph, false);
            match p.directive_callback {
                CallbackArg::Keep => {}
                CallbackArg::Null => m.set_directive_callback(0),
                CallbackArg::Set(id) => m.set_directive_callback(id),
            }
            match p.content_callback {
                CallbackArg::Keep => {}
                CallbackArg::Null => m.set_content_callback(None),
                CallbackArg::Set(handle) => m.set_content_callback(Some((handle, creation_type))),
            }
            if p.set_personality {
                m.set_personality(w, h, p.personality);
            }
            OpRType::SUCCESS
        }
        Op::SetZoneBehaviorParams(z) => {
            let mut zone = UserZone {
                mode: 0,
                invert: 0,
                target_point: Vector::ZERO,
                radius: 0.0,
                box_rotation: Matrix::IDENTITY,
                box_center: Vector::ZERO,
                box_half_extent: Vector::ZERO,
                target_key: 0,
            };
            match z.invert_mode {
                1 => zone.invert = 1,
                2 => {
                    m.state.pathfind.user_zone = None;
                    return OpRType::SUCCESS;
                }
                _ => {}
            }
            match z.mode {
                0 => {
                    zone.target_point = z.target_point;
                    zone.radius = z.radius;
                }
                1 => {
                    zone.mode = 1;
                    let center =
                        |max: f32, min: f32| ((f64::from(max) + f64::from(min)) * 0.5) as f32;
                    let half = |min: f32, c: f32| (f64::from(min) - f64::from(c)).abs() as f32;
                    let (lo, hi) = (z.box_min, z.box_max);
                    let c = Vector::new(center(hi.x, lo.x), center(hi.y, lo.y), center(hi.z, lo.z));
                    zone.box_center = c;
                    zone.box_half_extent =
                        Vector::new(half(lo.x, c.x), half(lo.y, c.y), half(lo.z, c.z));
                }
                2 => {
                    zone.mode = 2;
                    zone.radius = z.radius;
                    zone.target_key = z.target_id;
                }
                _ => {}
            }
            m.state.pathfind.user_zone = Some(zone);
            OpRType::FOUR
        }
        Op::SetFlagParams(f) => {
            match f.flag_id {
                0 => m.state.pathfind.flag_245 = f.value,
                1 => m.state.sys.guns.enabled = f.value != 0,
                _ => {}
            }
            OpRType::FOUR
        }
        _ => OpRType::FOUR,
    })
    .unwrap_or(OpRType::FOUR)
}

/// The `SETDIRECTIVE` record of one of the six kinds a client takes
/// (`FPayloadServerSetDirective`, built as `SubmitDirective`'s player
/// branch builds it); `None` for any other op.
#[must_use]
pub fn player_directive(op: &Op) -> Option<Directive> {
    let Op::Directive(d) = op else {
        return None;
    };
    let mut out = Directive::default();
    match d {
        DirectiveOp::Cancel => out.field_00 = 0,
        DirectiveOp::Delay(seconds) => {
            out.field_0c = *seconds;
            out.field_00 = 1;
        }
        DirectiveOp::Dock(k) => {
            out.field_10 = k.dock_object;
            out.field_14 = i32::from_ne_bytes(k.field_10.to_ne_bytes());
            out.field_08 = k.field_14;
            out.field_18 = k.port;
            out.field_1c = k.field_1c;
            out.field_20 = k.field_20;
            out.field_24 = k.field_24;
            out.field_00 = 2;
        }
        DirectiveOp::Follow(f) => {
            out.field_28 = f.target;
            out.field_2c = f.distance;
            out.field_30 = [f.offset.x, f.offset.y, f.offset.z];
            out.field_3c = f.range;
            out.field_00 = 3;
        }
        DirectiveOp::Idle => out.field_00 = 5,
        DirectiveOp::Goto(g) => {
            out.field_40 = g.kind;
            out.field_44 = [g.destination.x, g.destination.y, g.destination.z];
            out.field_50 = g.field_1c;
            for (block, words) in out.blocks.iter_mut().zip(g.words_20.as_chunks::<3>().0) {
                *block = *words;
            }
            out.field_84 = g.range;
            out.field_88 = g.field_54;
            out.tail = [g.field_58, g.field_59, g.cruise, g.no_cruise, g.field_6c];
            out.field_1c = g.field_5c;
            out.field_20 = g.field_60;
            out.field_24 = g.field_64;
            out.field_00 = 4;
        }
        _ => return None,
    }
    Some(out)
}

/// `SubmitDirective(Flid, Op)`: a directive for the object.
///
/// 4 for a zero id; 5 for an op that fails [`validate`], a missing object
/// or one in transit (no object database). On the ship a live player
/// flies, the six kinds of [`player_directive`] go to the owner's client
/// (1) and any other op is refused (5). Otherwise the object's manager:
/// a wait for a player's manoeuvre becomes directive kind 2 with the
/// behaviour's id (1); a cancel clears the directive (1); a current
/// directive blocks every other op (2); an op with a behaviour starts it
/// (1), anything else is refused (5). 5 for an object without a manager
/// (D711).
pub fn submit_directive(cx: &mut Cx<'_>, flid: u32, op: &BaseOp) -> OpRType {
    if flid & 0x3fff_ffff == 0 {
        return OpRType::FOUR;
    }
    if !validate(op) {
        return OpRType::INVALID;
    }
    let Some(h) = lookup_live(cx, flid) else {
        return OpRType::INVALID;
    };
    if cx.world.in_transit(h) {
        return OpRType::INVALID;
    }
    let owner = cx.world.get(h).map_or(0, |o| o.simple.owner_player);
    if is_valid_ship_owner(cx, owner, flid) {
        let Some(directive) = player_directive(&op.op) else {
            return OpRType::INVALID;
        };
        client::set_directive(cx, owner, directive);
        return OpRType::ACCEPTED;
    }
    let priority = i32::from_ne_bytes(op.priority.to_ne_bytes());
    with_manager(cx, h, |m, w| match &op.op {
        Op::WaitForPlayerManeuver(name) => {
            let id = behavior_type_id(name);
            if id == -1 {
                return OpRType::INVALID;
            }
            m.set_current_directive(w, h, [2, priority, id], None);
            OpRType::ACCEPTED
        }
        Op::Directive(d) => OpRType(m.submit_directive(w, h, d, priority)),
        _ if m.state.directive[0] != 0 => OpRType::DIRECTIVE_BUSY,
        _ => OpRType::INVALID,
    })
    .unwrap_or(OpRType::INVALID)
}

/// `set_directive_priority(Flid, Priority)`: the manager's answer, 1 with
/// a current directive (its priority changed), 3 without; 5 without the
/// object or a manager. The id is not masked.
pub fn set_directive_priority(cx: &mut Cx<'_>, flid: u32, priority: i32) -> OpRType {
    on_manager(cx, flid, |m, _, _| {
        OpRType(m.set_directive_priority(priority))
    })
    .unwrap_or(OpRType::INVALID)
}

/// `get_behavior_id(Flid)`: the manager's current behaviour, -1 without
/// the object or a manager.
pub fn behavior_id(cx: &mut Cx<'_>, flid: u32) -> i32 {
    on_manager(cx, flid, |m, _, _| m.state.behavior).unwrap_or(-1)
}

/// `get_state_graph_id(Flid)`: the manager's state graph, -1 without the
/// object, a manager or a graph.
pub fn state_graph_id(cx: &mut Cx<'_>, flid: u32) -> i32 {
    on_manager(cx, flid, |m, _, _| m.state.state_graph_id()).unwrap_or(-1)
}

/// `get_personality(Flid, &Out)`: a copy of the manager's personality;
/// `None` (false, nothing written) without the object or a manager.
pub fn personality(cx: &mut Cx<'_>, flid: u32) -> Option<Personality> {
    on_manager(cx, flid, |m, _, _| m.state.personality)
}

/// `enable_maneuver(Flid, Maneuver, Enable)`: true whenever the manager
/// exists, whatever its own answer (a manoeuvre of -1 is refused there).
pub fn enable_maneuver(cx: &mut Cx<'_>, flid: u32, maneuver: i32, enable: bool) -> bool {
    on_manager(cx, flid, |m, _, _| {
        m.enable_maneuver(maneuver, enable);
    })
    .is_some()
}

/// `enable_all_maneuvers(Flid)`: the manager's own answer, false without
/// the object or a manager.
pub fn enable_all_maneuvers(cx: &mut Cx<'_>, flid: u32) -> bool {
    on_manager(cx, flid, |m, _, _| m.enable_all_maneuvers()).unwrap_or(false)
}

/// `lock_maneuvers(Flid, Lock)`: true whenever the manager exists.
pub fn lock_maneuvers(cx: &mut Cx<'_>, flid: u32, lock: bool) -> bool {
    on_manager(cx, flid, |m, _, _| {
        m.lock_maneuvers(lock);
    })
    .is_some()
}

/// `get_scan_response(Scanner, Target, Good)`: the target is scanned. The
/// ship a live player flies tells its owner (`SCANNOTIFY` with the scanner
/// and the good) and answers 3; an AI answers from its target manager (1
/// when the hostile power around it is below its loot-flee threshold,
/// else 0). 0 without the target or a manager.
pub fn scan_response(cx: &mut Cx<'_>, scanner: u32, target: u32, good: u32) -> i32 {
    let Some(h) = lookup_live(cx, target) else {
        return 0;
    };
    let owner = cx.world.get(h).map_or(0, |o| o.simple.owner_player);
    if is_valid_ship_owner(cx, owner, target) {
        client::scan_notify(cx, owner, scanner, good);
        return 3;
    }
    with_manager(cx, h, |m, w| m.scan_response(w, h)).unwrap_or(0)
}

/// `submit_forced_target(Ship, Target)`: the target manager's answer, 1
/// when added, 0 when its eight are full, it is there already, or the
/// ship or its manager is missing.
pub fn submit_forced_target(cx: &mut Cx<'_>, ship: u32, target: u32) -> i32 {
    on_manager(cx, ship, |m, _, _| {
        m.state.sys.targeting.submit_forced_target(target)
    })
    .unwrap_or(0)
}

/// `remove_forced_target(Ship, Target)`: 1 when removed, 0 when not there
/// or the ship or its manager is missing.
pub fn remove_forced_target(cx: &mut Cx<'_>, ship: u32, target: u32) -> i32 {
    on_manager(cx, ship, |m, _, _| {
        m.state.sys.targeting.remove_forced_target(target)
    })
    .unwrap_or(0)
}

/// `set_player_enemy_clamp(Ship, Min, Max)`: the target manager's bounds
/// (D172); false without the ship or its manager.
pub fn set_player_enemy_clamp(cx: &mut Cx<'_>, ship: u32, min: i32, max: i32) -> bool {
    on_manager(cx, ship, |m, _, _| {
        m.state.sys.targeting.set_player_enemy_clamp(min, max);
    })
    .is_some()
}

/// The ship's live formation leader (`get_formation_leader`).
fn formation_leader(cx: &Cx<'_>, h: ObjHandle) -> Option<ObjHandle> {
    cx.world
        .get(h)
        .and_then(|o| o.as_ship())
        .and_then(|s| s.formation_leader)
        .filter(|&l| cx.world.get(l).is_some())
}

/// The leader's live formation followers (`get_formation_followers`), in
/// slot order.
fn formation_followers(cx: &Cx<'_>, h: ObjHandle) -> Vec<ObjHandle> {
    cx.world
        .get(h)
        .and_then(|o| o.as_ship())
        .map(|s| {
            s.formation_followers
                .iter()
                .flatten()
                .copied()
                .filter(|&f| cx.world.get(f).is_some())
                .collect()
        })
        .unwrap_or_default()
}

fn object_id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).map_or(0, |o| o.simple.id)
}

/// `Formation_SendIdsToOwner`: a player's ship tells its owner the
/// formation's members.
fn send_ids_to_owner(cx: &mut Cx<'_>, h: ObjHandle, ids: &[u32]) {
    let Some(owner) = cx
        .world
        .get(h)
        .filter(|o| o.kind == ObjectKind::Ship && o.as_ship().is_some_and(|s| s.is_player))
        .map(|o| o.simple.owner_player)
    else {
        return;
    };
    client::misc_obj_update_formation(cx, owner, ids);
}

/// `update_formation_state(Ship, Leader, Offset)`: the ship leaves its old
/// leader's formation (unless it led itself), takes the new leader (none
/// for a missing one) and the offset, and joins the new leader's followers
/// (unless it is the leader), through [`World::update_formation_state`].
/// On a multiplayer server an unchanged leader changes nothing, and the
/// players of the old formation (its leader's id first when it has
/// followers, then the followers but the ship) and of the new one (its
/// leader, then every follower) are told its members, the followers
/// first.
///
/// # Errors
///
/// -3 when the ship is not there.
pub fn update_formation_state(
    cx: &mut Cx<'_>,
    ship: u32,
    leader: u32,
    offset: Vector,
) -> Result<(), PubError> {
    let Some(h) = lookup_live(cx, ship) else {
        return Err(PubError::NOT_FOUND);
    };
    let old = formation_leader(cx, h);
    let new = lookup_live(cx, leader);
    let single_player = cx.world.config().single_player;
    if !single_player && new == old {
        return Ok(());
    }
    if !single_player && let Some(old) = old {
        let followers = formation_followers(cx, old);
        let own = object_id(cx, h);
        let mut ids = Vec::new();
        if !followers.is_empty() {
            ids.push(object_id(cx, old));
        }
        ids.extend(
            followers
                .iter()
                .map(|&f| object_id(cx, f))
                .filter(|&id| id != own),
        );
        for &f in &followers {
            send_ids_to_owner(cx, f, &ids);
        }
        send_ids_to_owner(cx, old, &ids);
    }
    cx.world.update_formation_state(h, new, offset);
    if !single_player && let Some(new) = new {
        let followers = formation_followers(cx, new);
        let mut ids = vec![object_id(cx, new)];
        ids.extend(followers.iter().map(|&f| object_id(cx, f)));
        for &f in &followers {
            send_ids_to_owner(cx, f, &ids);
        }
        send_ids_to_owner(cx, new, &ids);
    }
    Ok(())
}

/// `refresh_state_graph(Flid)`: the manager takes its state graph again
/// (`set_state_graph(id, true)`); nothing without the object or a manager.
pub fn refresh_state_graph(cx: &mut Cx<'_>, flid: u32) {
    let data = cx.world.data_arc();
    on_manager(cx, flid, |m, w, h| {
        m.refresh_state_graph(w, h, &data.state_graphs);
    });
}
