//! The NPC guides (TU37, `Content/Guides.cpp`, `GotoGuides.cpp`,
//! `PatrolGuides.cpp`; record 0008 point 7; work package P3.6c and its
//! follow-up): the long-running behaviour of an NPC ship (`FContentNpcShip
//! +0x178`), which gives the ship `[ObjList]` directives and hears its
//! events.
//!
//! **Guides.** A guide is a counted object (`FContentGuide`) in Content's
//! arena ([`GuideRef`]): the NPC ship holds one reference, a guide the
//! ship's formation shares (`PromoteFormationLeader`) another, and
//! `BreakFormationAtDock` gives each NPC member a clone (`Clone`, a
//! member-wise copy that copies the reference count too).
//! [`create_guide`] (`CreateGuide`) builds one from a [`GuideDesc`] by its
//! type:
//! - 1, nothing (`FNothingGuide`): the ship waits 10000 s at a time;
//! - 2, trade ([`trade`]): to the second target and back, docking at each;
//! - 3, patrol path ([`patrol`]): the encounter's zone path between the
//!   targets, with the cargo scanner;
//! - 4, wander ([`wander`]): from point to point around the ship;
//! - 5 and 6, the goto and dock guides of a `[MetaBehavior]` ([`goto`]).
//!
//! Types 1 to 4 are target guides ([`target`]): they copy the
//! description's two targets, and the nothing and wander guides answer the
//! flee from them.
//!
//! **The calls.** The C++'s virtual slots are matches on [`GuideKind`]. A
//! slot works on a copy of the guide that is written back when it returns
//! (nothing the guide calls reaches the same guide again before then).
//! The flee answer is computed on a copy that is dropped before the
//! behaviour tick, and made again with its effects after it (record 0008
//! point 4, D852).
//!
//! **Directives.** [`execute_directive`] is `Execute` (slot 3) of the
//! directive kinds the guides, the flee answer and the space paths make:
//! the delay, idle, goto, trail, guide, dock, launch and tractor beam
//! directives. A directive that names a target (`FindObjectIdByName`)
//! comes only from a mission's `[ObjList]`, whose run time is P3.5c's: such
//! a directive finishes at once (D855). The directives are shared objects
//! ([`super::directives`]).

use std::collections::BTreeMap;

use game_sim::ai::behavior::DirectiveOp;
use game_sim::ai::content::FleeAnswer;
use neuron_core::math::Vector;

use super::directives::{DirectiveList, DirectiveRef};
use super::mission::meta_behavior::MetaBehavior;
use super::mission::obj_list::{Delay, DirectiveKind, Dock};
use super::npc_ships::{self, set_goto_destination};
use super::ships::{self, ShipKey};
use crate::api;
use crate::api::ai::BaseOp;
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

pub mod goto;
pub mod patrol;
pub mod target;
pub mod trade;
pub mod wander;

/// `GGuideLaneEntryRangeSquared` (`Guides.cpp:28`): 10000², the reach of
/// the trade and patrol guides' flee answers.
pub const LANE_ENTRY_RANGE_SQUARED: f32 = 10000.0 * 10000.0;

/// A system record's patrol path (`FPatrolPath*`), by the record and the
/// path's index in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PatrolPathRef {
    /// The system record.
    pub system: u32,
    /// The index in its patrol paths.
    pub index: usize,
}

/// The placement plan of an encounter spawn (`FSpawnPlan`, 0x38 bytes),
/// which TU53 fills and copies into the leader's description (P3.6d).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpawnPlan {
    /// `Kind` (`+0x00`): 0 not placed, 1 a lane, 2 a dock launch, 7 a
    /// cruise, 8 a buzz.
    pub kind: i32,
    /// `RingId` (`+0x04`).
    pub ring: u32,
    /// `NextRingId` (`+0x08`).
    pub next_ring: u32,
    /// `DockTarget` (`+0x0c`).
    pub dock_target: u32,
    /// `Position` (`+0x10`).
    pub position: Vector,
    /// `Destination` (`+0x1c`).
    pub destination: Vector,
    /// `Path` (`+0x28`): a patrol path (guide type 3).
    pub path: Option<PatrolPathRef>,
    /// `PathPoint` (`+0x2c`, -1): the point of the trade target, the
    /// patrol's end point.
    pub path_point: i32,
    /// `PathTargetPoint` (`+0x30`, -1): the point the ships head for.
    pub path_target_point: i32,
    /// `PathDirection` (`+0x34`, 0): +1 or -1 along the path.
    pub path_direction: i32,
}

impl Default for SpawnPlan {
    fn default() -> Self {
        Self {
            kind: 0,
            ring: 0,
            next_ring: 0,
            dock_target: 0,
            position: Vector::ZERO,
            destination: Vector::ZERO,
            path: None,
            path_point: -1,
            path_target_point: -1,
            path_direction: 0,
        }
    }
}

/// A guide description: `FGuideDesc` (types 1 to 4) or a `[MetaBehavior]`
/// object (types 5 and 6, `FMbGotoGuide`, `FMbDockGuide`), as the guide
/// constructors read it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuideDesc {
    /// `Type` (`+0x08`): 0 none, 1 to 6 the guide kinds.
    pub kind: i32,
    /// `Plan` (`+0x10`).
    pub plan: SpawnPlan,
    /// `WanderZone` (`+0x48`): the wander guide's zone.
    pub wander_zone: Option<u32>,
    /// `TradeGood` (`+0x4c`).
    pub trade_good: u32,
    /// `ReturnGood` (`+0x50`).
    pub return_good: u32,
    /// `TargetId1` (`+0x54`).
    pub target1: u32,
    /// `TargetId2` (`+0x58`).
    pub target2: u32,
    /// `TargetBase1` (`+0x5c`), by base id.
    pub base1: Option<u32>,
    /// `TargetBase2` (`+0x60`), by base id.
    pub base2: Option<u32>,
    /// The `[MetaBehavior]` object of types 5 and 6.
    pub meta: Option<MetaBehavior>,
}

impl GuideDesc {
    /// The description of a `[MetaBehavior]` object (type 5 or 6).
    #[must_use]
    pub fn from_meta(meta: MetaBehavior) -> Self {
        Self {
            kind: meta.type_code(),
            meta: Some(meta),
            ..Self::default()
        }
    }

    /// `FGuideDesc::SetTargets(&Id1, &Id2, &ClassIds, &Rep)`
    /// (`Guides.cpp:990-997`): the two targets and the bases reached from
    /// them ([`find_target_base`]), the first's first.
    pub fn set_targets(&mut self, cx: &mut Cx<'_>, id1: u32, id2: u32, vibe: i32) {
        self.target1 = id1;
        self.target2 = id2;
        self.base1 = find_target_base(cx, id1, vibe);
        self.base2 = find_target_base(cx, id2, vibe);
    }
}

/// `FGuideDesc::FindTargetBase(&ObjectId, ClassIds, &Rep)`
/// (`Guides.cpp:1001-1036`): through a jump gate or hole (type `0x40`) a
/// random base of the destination system (at most 20, `pub::GetBases`)
/// whose record has the vibe's affiliation among its factions (each refused
/// one leaves the candidates), else the object's docking target; the base
/// record of it (`FindContentBaseInfo`), none without one. The class list
/// is not read.
pub fn find_target_base(cx: &mut Cx<'_>, object: u32, vibe: i32) -> Option<u32> {
    let mut base = 0;
    let kind = api::space_obj::object_type(cx, object).unwrap_or(0);
    if kind & 0x40 != 0 {
        let (system, _) = api::space_obj::jump_target(cx, object).unwrap_or((0, 0));
        let mut candidates = api::misc::bases(cx, system, 0x14);
        while !candidates.is_empty() && base == 0 {
            let pick = cx.world.rng().rand().wrapping_mul(candidates.len() as i32) / 32768;
            let at = usize::try_from(pick).unwrap_or(0).min(candidates.len() - 1);
            if super::space_paths::is_base_of_affiliation(cx, vibe, candidates[at]) {
                base = candidates[at];
            } else {
                candidates.remove(at);
            }
        }
    } else {
        base = api::space_obj::docking_target(cx, object).unwrap_or(0);
    }
    cx.logic
        .content
        .state
        .bases
        .contains_key(&base)
        .then_some(base)
}

/// A guide in Content's arena (`FContentGuide`, counted).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GuideRef(pub u32);

/// `FTargetGuide`'s two targets (`+0x14` to `+0x20`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Targets {
    /// `TargetId1`.
    pub id1: u32,
    /// `TargetId2`.
    pub id2: u32,
    /// `TargetBase1`.
    pub base1: Option<u32>,
    /// `TargetBase2`.
    pub base2: Option<u32>,
}

impl Targets {
    /// `FTargetGuide::SwapTargets` (`Guides.cpp:282-290`).
    pub fn swap(&mut self) {
        std::mem::swap(&mut self.id1, &mut self.id2);
        std::mem::swap(&mut self.base1, &mut self.base2);
    }
}

/// The guide's class and its members.
#[derive(Clone, Debug, PartialEq)]
pub enum GuideKind {
    /// Type 1, `FNothingGuide` (0x28): the target guide and a 10000 s
    /// delay directive (`+0x24`), one reference held.
    Nothing {
        /// The targets.
        targets: Targets,
        /// `DelayDirective`.
        delay: DirectiveRef,
    },
    /// Type 2, `FTradeGuide`.
    Trade(trade::TradeGuide),
    /// Type 3, `FPatrolGuide`.
    Patrol(Box<patrol::PatrolGuide>),
    /// Type 4, `FWanderGuide`.
    Wander(wander::WanderGuide),
    /// Type 5, `FGotoGuide`.
    Goto(Box<goto::GotoGuide>),
    /// Type 6, `FDockGuide`.
    Dock(Box<goto::GotoGuide>, goto::DockPart),
}

/// `FContentGuide` (the base, `+0x08` to `+0x10`) with its class.
#[derive(Clone, Debug, PartialEq)]
pub struct Guide {
    /// `Ship` (`+0x08`).
    pub ship: Option<NpcShipKey>,
    /// `Type` (`+0x0c`).
    pub kind_id: i32,
    /// `bDone` (`+0x10`).
    pub done: bool,
    /// The class.
    pub kind: GuideKind,
}

/// The arena (`ContentState::guides`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuideArena {
    next: u32,
    entries: BTreeMap<GuideRef, (u32, Guide)>,
    /// The guides whose slot runs on a copy now ([`with_guide`]).
    running: Vec<GuideRef>,
    /// The messages that reached a running guide, delivered when its slot
    /// returns (D860).
    deferred: Vec<(GuideRef, GuideMessage)>,
}

/// A message a guide reads, kept while the guide's slot runs (D860).
#[derive(Clone, Debug, PartialEq)]
pub enum GuideMessage {
    /// `CommComplete` (0x0b).
    CommComplete {
        /// The listener.
        listener: u32,
        /// The system.
        system: u32,
        /// The speaker.
        speaker: u32,
        /// The line.
        line: u32,
        /// The result.
        result: crate::events::CommResult,
    },
    /// `DockComplete` (0x11).
    DockComplete {
        /// The ship.
        ship: u32,
        /// The dock.
        dock: u32,
        /// The system.
        system: u32,
    },
    /// `TradelaneRequest` (0x14).
    TradelaneRequest {
        /// The ship.
        ship: u32,
        /// The ring.
        ring: u32,
        /// The end ring.
        end_ring: u32,
        /// The system.
        system: u32,
    },
    /// `TradelaneEnter` (0x16).
    TradelaneEnter {
        /// The ship.
        ship: u32,
        /// The entrance.
        entrance: u32,
        /// The exit.
        exit: u32,
        /// The system.
        system: u32,
    },
    /// `TradelaneExit` (0x17).
    TradelaneExit {
        /// The ship.
        ship: u32,
        /// The entrance.
        entrance: u32,
        /// The exit.
        exit: u32,
        /// The system.
        system: u32,
    },
    /// `LootAcquired` (0x18).
    LootAcquired {
        /// The loot.
        loot: u32,
        /// The ship.
        ship: u32,
        /// The sub-object.
        sub_object: u16,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// `FormationRequest` (0x19).
    FormationRequest {
        /// The ship.
        ship: u32,
        /// The leader.
        leader: u32,
        /// The system.
        system: u32,
    },
    /// `Heartbeat` (0x20).
    Heartbeat {
        /// The elapsed time.
        elapsed: f32,
    },
    /// `CargoJettisoned` (0x23).
    CargoJettisoned {
        /// The ship.
        ship: u32,
        /// The sub-object.
        equip: u16,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// `Msg0x27`.
    Msg0x27 {
        /// The ship.
        ship: u32,
        /// The base.
        base: u32,
        /// The target.
        target: u32,
        /// Landing.
        land: bool,
    },
    /// `LootSpawned` (0x2b).
    LootSpawned {
        /// The loot.
        loot: u32,
        /// Its source.
        source: u32,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// `ObjectDestroyed` (0x39).
    ObjectDestroyed(crate::events::ObjectDestroyed),
    /// `SystemExit` (0x3a).
    SystemExit {
        /// The system.
        system: u32,
        /// The destination.
        destination: u32,
        /// The player.
        player: u32,
    },
    /// `CombatState` (0x1001).
    CombatState {
        /// The state.
        state: u32,
        /// The first id.
        param1: u32,
        /// The second id.
        param2: u32,
        /// The system.
        system: u32,
    },
    /// Any other message, which no guide reads but whose arrival the
    /// patrol guide answers by its scanner's status; delivered as
    /// `MissionStart`, which no guide reads either.
    Other,
}

impl GuideMessage {
    /// The copy of a message a guide reads ([`GuideMessage::Other`] for
    /// the others).
    fn of(message: &ControllerMessage<'_>) -> Self {
        use ControllerMessage as M;
        match message {
            M::CommComplete {
                listener,
                system,
                speaker,
                line,
                result,
            } => Self::CommComplete {
                listener: *listener,
                system: *system,
                speaker: *speaker,
                line: *line,
                result: *result,
            },
            M::DockComplete { ship, dock, system } => Self::DockComplete {
                ship: *ship,
                dock: *dock,
                system: *system,
            },
            M::TradelaneRequest {
                ship,
                ring,
                end_ring,
                system,
            } => Self::TradelaneRequest {
                ship: *ship,
                ring: *ring,
                end_ring: *end_ring,
                system: *system,
            },
            M::TradelaneEnter {
                ship,
                entrance,
                exit,
                system,
            } => Self::TradelaneEnter {
                ship: *ship,
                entrance: *entrance,
                exit: *exit,
                system: *system,
            },
            M::TradelaneExit {
                ship,
                entrance,
                exit,
                system,
            } => Self::TradelaneExit {
                ship: *ship,
                entrance: *entrance,
                exit: *exit,
                system: *system,
            },
            M::LootAcquired {
                loot,
                ship,
                sub_object,
                good,
                count,
            } => Self::LootAcquired {
                loot: *loot,
                ship: *ship,
                sub_object: *sub_object,
                good: *good,
                count: *count,
            },
            M::FormationRequest {
                ship,
                leader,
                system,
            } => Self::FormationRequest {
                ship: *ship,
                leader: *leader,
                system: *system,
            },
            M::Heartbeat { elapsed } => Self::Heartbeat { elapsed: *elapsed },
            M::CargoJettisoned {
                ship,
                equip,
                good,
                count,
            } => Self::CargoJettisoned {
                ship: *ship,
                equip: *equip,
                good: *good,
                count: *count,
            },
            M::Msg0x27 {
                ship,
                base,
                target,
                land,
            } => Self::Msg0x27 {
                ship: *ship,
                base: *base,
                target: *target,
                land: *land,
            },
            M::LootSpawned {
                loot,
                source,
                good,
                count,
            } => Self::LootSpawned {
                loot: *loot,
                source: *source,
                good: *good,
                count: *count,
            },
            M::ObjectDestroyed(event) => Self::ObjectDestroyed(event.clone()),
            M::SystemExit {
                system,
                destination,
                player,
            } => Self::SystemExit {
                system: *system,
                destination: *destination,
                player: *player,
            },
            M::CombatState {
                state,
                param1,
                param2,
                system,
            } => Self::CombatState {
                state: *state,
                param1: *param1,
                param2: *param2,
                system: *system,
            },
            _ => Self::Other,
        }
    }

    /// The message again.
    fn message(self) -> ControllerMessage<'static> {
        use ControllerMessage as M;
        match self {
            Self::CommComplete {
                listener,
                system,
                speaker,
                line,
                result,
            } => M::CommComplete {
                listener,
                system,
                speaker,
                line,
                result,
            },
            Self::DockComplete { ship, dock, system } => M::DockComplete { ship, dock, system },
            Self::TradelaneRequest {
                ship,
                ring,
                end_ring,
                system,
            } => M::TradelaneRequest {
                ship,
                ring,
                end_ring,
                system,
            },
            Self::TradelaneEnter {
                ship,
                entrance,
                exit,
                system,
            } => M::TradelaneEnter {
                ship,
                entrance,
                exit,
                system,
            },
            Self::TradelaneExit {
                ship,
                entrance,
                exit,
                system,
            } => M::TradelaneExit {
                ship,
                entrance,
                exit,
                system,
            },
            Self::LootAcquired {
                loot,
                ship,
                sub_object,
                good,
                count,
            } => M::LootAcquired {
                loot,
                ship,
                sub_object,
                good,
                count,
            },
            Self::FormationRequest {
                ship,
                leader,
                system,
            } => M::FormationRequest {
                ship,
                leader,
                system,
            },
            Self::Heartbeat { elapsed } => M::Heartbeat { elapsed },
            Self::CargoJettisoned {
                ship,
                equip,
                good,
                count,
            } => M::CargoJettisoned {
                ship,
                equip,
                good,
                count,
            },
            Self::Msg0x27 {
                ship,
                base,
                target,
                land,
            } => M::Msg0x27 {
                ship,
                base,
                target,
                land,
            },
            Self::LootSpawned {
                loot,
                source,
                good,
                count,
            } => M::LootSpawned {
                loot,
                source,
                good,
                count,
            },
            Self::ObjectDestroyed(event) => M::ObjectDestroyed(event),
            Self::SystemExit {
                system,
                destination,
                player,
            } => M::SystemExit {
                system,
                destination,
                player,
            },
            Self::CombatState {
                state,
                param1,
                param2,
                system,
            } => M::CombatState {
                state,
                param1,
                param2,
                system,
            },
            Self::Other => M::MissionStart,
        }
    }
}

impl GuideArena {
    fn insert(&mut self, refs: u32, guide: Guide) -> GuideRef {
        self.next += 1;
        let key = GuideRef(self.next);
        self.entries.insert(key, (refs, guide));
        key
    }

    /// The guide.
    #[must_use]
    pub fn get(&self, key: GuideRef) -> Option<&Guide> {
        self.entries.get(&key).map(|(_, g)| g)
    }

    fn get_mut(&mut self, key: GuideRef) -> Option<&mut Guide> {
        self.entries.get_mut(&key).map(|(_, g)| g)
    }

    /// `AddRef`.
    pub fn add_ref(&mut self, key: GuideRef) {
        if let Some(e) = self.entries.get_mut(&key) {
            e.0 += 1;
        }
    }

    /// The reference count (for tests).
    #[must_use]
    pub fn refs(&self, key: GuideRef) -> u32 {
        self.entries.get(&key).map_or(0, |e| e.0)
    }
}

/// The ship's position (`GetPosition`, `pub::SpaceObj::GetLocation`).
#[must_use]
pub fn ship_position(cx: &Cx<'_>, ship: NpcShipKey) -> Vector {
    ships::position(cx, ShipKey::Npc(ship))
}

/// A slot on a copy of the guide, written back when it returns (unless the
/// guide is gone by then); `None` for a guide that is gone. A message that
/// reaches the guide meanwhile (a server call of the slot that dispatches
/// one, as the scanner's jettison does) waits until then (D860).
fn with_guide<R>(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    f: impl FnOnce(&mut Cx<'_>, &mut Guide) -> R,
) -> Option<R> {
    let mut g = cx.logic.content.state.guides.get(guide)?.clone();
    cx.logic.content.state.guides.running.push(guide);
    let r = f(cx, &mut g);
    let arena = &mut cx.logic.content.state.guides;
    if let Some(at) = arena.running.iter().rposition(|&r| r == guide) {
        arena.running.remove(at);
    }
    if let Some(slot) = arena.get_mut(guide) {
        *slot = g;
    }
    if !arena.running.contains(&guide) {
        while let Some(at) = cx
            .logic
            .content
            .state
            .guides
            .deferred
            .iter()
            .position(|(r, _)| *r == guide)
        {
            let (_, message) = cx.logic.content.state.guides.deferred.remove(at);
            handle_event(cx, guide, &message.message());
        }
    }
    Some(r)
}

/// `Release`: the last one deletes the guide (its destructor).
pub fn release(cx: &mut Cx<'_>, guide: GuideRef) {
    let arena = &mut cx.logic.content.state.guides;
    let Some(e) = arena.entries.get_mut(&guide) else {
        return;
    };
    e.0 = e.0.saturating_sub(1);
    if e.0 != 0 {
        return;
    }
    let Some((_, mut g)) = arena.entries.remove(&guide) else {
        return;
    };
    let ship = g.ship;
    match &mut g.kind {
        GuideKind::Nothing { delay, .. } => cx.logic.content.state.directives.release(*delay),
        GuideKind::Trade(_) => {}
        GuideKind::Patrol(p) => {
            // `~FPatrolGuide` (`PatrolGuides.cpp:939-945`).
            if let Some(s) = ship {
                super::space_support::unregister_guided_ship(cx, s);
            }
            p.scanner.destroy(cx, ship);
        }
        GuideKind::Wander(w) => {
            if let Some(d) = w.directive {
                cx.logic.content.state.directives.release(d);
            }
        }
        GuideKind::Goto(go) | GuideKind::Dock(go, _) => goto::destroy(cx, ship, go),
    }
}

/// `CreateGuide(Desc, &Guide)` (`Guides.cpp:221-261`): the guide of the
/// description's type for `ship`, with one reference; `None` for any other
/// type. A type 5 or 6 without its `[MetaBehavior]` object, which the C++
/// reads as one, builds none, and a type 6 from a goto guide's object has
/// the dock target 0 (D864).
pub fn create_guide(cx: &mut Cx<'_>, desc: &GuideDesc, ship: NpcShipKey) -> Option<GuideRef> {
    let targets = Targets {
        id1: desc.target1,
        id2: desc.target2,
        base1: desc.base1,
        base2: desc.base2,
    };
    let kind = match desc.kind {
        1 => {
            // The delay is made with the directive's own constructor and
            // assigned through `AssignDirective`, which takes a reference:
            // the creating one is never released (two references).
            let arena = &mut cx.logic.content.state.directives;
            let delay = arena.create(DirectiveKind::Delay(Delay {
                priority: 1,
                delay_seconds: 10000.0,
            }));
            arena.add_ref(delay);
            GuideKind::Nothing { targets, delay }
        }
        2 => GuideKind::Trade(trade::TradeGuide::new(cx, desc, targets)),
        3 => GuideKind::Patrol(Box::new(patrol::PatrolGuide::new(
            cx,
            desc,
            targets,
            Some(ship),
        ))),
        4 => GuideKind::Wander(wander::WanderGuide::new(desc, targets)),
        5 => {
            let mb = desc.meta.as_ref()?.guide();
            GuideKind::Goto(Box::new(goto::GotoGuide::new(cx, mb, Some(ship))))
        }
        6 => {
            let meta = desc.meta.as_ref()?;
            let dock_target = match meta {
                MetaBehavior::DockGuide(_, target) => *target,
                MetaBehavior::GotoGuide(_) => 0,
            };
            let (go, dock) = goto::new_dock(cx, meta.guide(), dock_target, Some(ship));
            GuideKind::Dock(Box::new(go), dock)
        }
        _ => return None,
    };
    let guide = Guide {
        ship: Some(ship),
        kind_id: desc.kind,
        done: false,
        kind,
    };
    Some(cx.logic.content.state.guides.insert(1, guide))
}

/// The guide part of `FContentNpcShip`'s constructor (`NpcShip.cpp:368-403`):
/// a description of type 0 drops the ship's guide; any other builds one
/// (`CreateGuide`) that starts when the ship has no directives and, with a
/// guide target, when the target's type has a bit of `0xc40`.
pub fn attach_from_params(cx: &mut Cx<'_>, ship: NpcShipKey, desc: &GuideDesc, target: u32) {
    if desc.kind == 0 {
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        return;
    }
    let mut start = true;
    if target != 0 {
        let kind = api::space_obj::object_type(cx, target).unwrap_or(0);
        if kind & 0xc40 == 0 {
            start = false;
        }
    }
    set_guide_from_desc(cx, ship, desc, start);
}

/// `SetGuideFromDesc(Desc, bStart)` (`NpcShip.cpp:900-917`): the old guide
/// (with the AI directive cancelled) replaced by one built from the
/// description, started when the ship has no directives.
pub fn set_guide_from_desc(cx: &mut Cx<'_>, ship: NpcShipKey, desc: &GuideDesc, start: bool) {
    if npc_ships::get(cx, ship).and_then(|s| s.guide).is_some() {
        cancel_ai_directive(cx, ship);
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
    }
    if desc.kind == 0 {
        return;
    }
    let made = create_guide(cx, desc, ship);
    if let Some(s) = npc_ships::get_mut(cx, ship) {
        s.guide = made;
    } else if let Some(g) = made {
        release(cx, g);
        return;
    }
    if start
        && npc_ships::has_no_directives(cx, ShipKey::Npc(ship))
        && let Some(g) = made
    {
        start_guide(cx, g);
    }
}

/// `SetGuide(NewGuide, bStart, bReset)` (`NpcShip.cpp:920-951`): the ship
/// takes the guide (one reference), the guide the ship; reset and started
/// as asked. `None` cancels the AI directive and drops the guide.
pub fn set_guide(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    guide: Option<GuideRef>,
    start: bool,
    reset: bool,
) {
    let Some(new) = guide else {
        if npc_ships::get(cx, ship).and_then(|s| s.guide).is_some() {
            cancel_ai_directive(cx, ship);
        }
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        return;
    };
    let current = npc_ships::get(cx, ship).and_then(|s| s.guide);
    if current != Some(new) {
        if let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take()) {
            release(cx, old);
        }
        cx.logic.content.state.guides.add_ref(new);
        match npc_ships::get_mut(cx, ship) {
            Some(s) => s.guide = Some(new),
            None => {
                release(cx, new);
                return;
            }
        }
    }
    set_ship(cx, new, Some(ship));
    if reset {
        reset_guide(cx, new);
    }
    if start && npc_ships::has_no_directives(cx, ShipKey::Npc(ship)) {
        start_guide(cx, new);
    }
}

/// `Clone` (slot 3): a member-wise copy, the reference count included;
/// the copy's directives are `AddRef`'ed. The patrol guide's scanner
/// leaves its two id lists behind, and the copy joins no guided ships.
pub fn clone_guide(cx: &mut Cx<'_>, guide: GuideRef) -> Option<GuideRef> {
    let (refs, mut copy) = cx
        .logic
        .content
        .state
        .guides
        .entries
        .get(&guide)
        .map(|(r, g)| (*r, g.clone()))?;
    let ship = copy.ship;
    match &mut copy.kind {
        GuideKind::Nothing { delay, .. } => cx.logic.content.state.directives.add_ref(*delay),
        GuideKind::Trade(t) => t.route.field_0x1c = 0,
        GuideKind::Patrol(p) => p.scanner = p.scanner.copy(cx),
        GuideKind::Wander(w) => {
            if let Some(d) = w.directive {
                cx.logic.content.state.directives.add_ref(d);
            }
        }
        GuideKind::Goto(go) => {
            let (g, _) = goto::copy(cx, ship, go, None);
            **go = g;
        }
        GuideKind::Dock(go, dock) => {
            let (g, d) = goto::copy(cx, ship, go, Some(dock));
            **go = g;
            if let Some(d) = d {
                *dock = d;
            }
        }
    }
    Some(cx.logic.content.state.guides.insert(refs, copy))
}

/// `SubmitDirective(SpaceObjId, DirectiveCancelOp)` on the ship.
fn cancel_ai_directive(cx: &mut Cx<'_>, ship: NpcShipKey) {
    let id = npc_ships::space_obj(cx, ship);
    let _ = api::ai::submit_directive(cx, id, &BaseOp::directive(DirectiveOp::Cancel));
}

/// `Start` (slot 4).
pub fn start_guide(cx: &mut Cx<'_>, guide: GuideRef) {
    let _ = with_guide(cx, guide, |cx, g| {
        let ship = g.ship;
        match &mut g.kind {
            GuideKind::Nothing { delay, .. } => target::issue_only_directive(cx, ship, *delay),
            GuideKind::Trade(t) => trade::start(cx, ship, t),
            GuideKind::Patrol(p) => patrol::start(cx, ship, p),
            GuideKind::Wander(w) => wander::start(cx, ship, w),
            GuideKind::Goto(go) => goto::start(cx, ship, go),
            GuideKind::Dock(go, dock) => goto::dock_start(cx, ship, go, dock),
        }
    });
}

/// `Reset` (slot 5): nothing for the nothing guide; the trade, patrol and
/// wander guides go back to state 0, the goto and dock guides to state 1.
pub fn reset_guide(cx: &mut Cx<'_>, guide: GuideRef) {
    if let Some(g) = cx.logic.content.state.guides.get_mut(guide) {
        match &mut g.kind {
            GuideKind::Nothing { .. } => {}
            GuideKind::Trade(t) => t.state = 0,
            GuideKind::Patrol(p) => p.state = 0,
            GuideKind::Wander(w) => w.state = 0,
            GuideKind::Goto(go) | GuideKind::Dock(go, _) => go.reset(),
        }
    }
}

/// `OnDirectivesDone(Word11c, Word118)` (slot 6): the target guides start
/// again; the goto and dock guides do nothing.
pub fn on_directives_done(cx: &mut Cx<'_>, guide: GuideRef, result: (i32, i32)) {
    let _ = result;
    let Some(g) = cx.logic.content.state.guides.get(guide) else {
        return;
    };
    match g.kind {
        GuideKind::Goto(_) | GuideKind::Dock(..) => {}
        _ => start_guide(cx, guide),
    }
}

/// `HandleEvent(MsgId, Parms)` (slot 7): the base's does nothing; the
/// patrol guide's goes to its scanner, the goto and dock guides' move
/// their states and start again when asked.
pub fn handle_event(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    message: &crate::events::ControllerMessage<'_>,
) {
    if cx.logic.content.state.guides.running.contains(&guide) {
        let m = GuideMessage::of(message);
        cx.logic.content.state.guides.deferred.push((guide, m));
        return;
    }
    let _ = with_guide(cx, guide, |cx, g| {
        let ship = g.ship;
        let Guide { kind, done, .. } = g;
        match kind {
            GuideKind::Nothing { .. } | GuideKind::Trade(_) | GuideKind::Wander(_) => {}
            GuideKind::Patrol(p) => patrol::handle_event(cx, ship, p, message),
            GuideKind::Goto(go) => {
                goto::handle_event(cx, ship, go, done, message);
                // `FinishEvent`.
                if go.restart {
                    goto::start(cx, ship, go);
                }
            }
            GuideKind::Dock(go, dock) => {
                if goto::dock_handle_event(cx, ship, go, dock, done, message) {
                    goto::dock_start(cx, ship, go, dock);
                }
            }
        }
    });
}

/// `SetShip(Ship)` (slot 8): the base's stores the ship; the patrol, goto
/// and dock guides move the guided ship and rebuild what depends on it.
pub fn set_ship(cx: &mut Cx<'_>, guide: GuideRef, ship: Option<NpcShipKey>) {
    let _ = with_guide(cx, guide, |cx, g| {
        let old = g.ship;
        g.ship = ship;
        match &mut g.kind {
            GuideKind::Nothing { .. } | GuideKind::Trade(_) | GuideKind::Wander(_) => {}
            GuideKind::Patrol(p) => patrol::set_ship(cx, old, ship, p),
            GuideKind::Goto(go) => goto::set_ship(cx, old, ship, go, None),
            GuideKind::Dock(go, dock) => goto::set_ship(cx, old, ship, go, Some(dock)),
        }
    });
}

/// `IsDone` (slot 9).
#[must_use]
pub fn is_done(cx: &Cx<'_>, guide: GuideRef) -> bool {
    cx.logic
        .content
        .state
        .guides
        .get(guide)
        .is_some_and(|g| g.done)
}

/// `GetType` (slot 10).
#[must_use]
pub fn guide_type(cx: &Cx<'_>, guide: GuideRef) -> i32 {
    cx.logic
        .content
        .state
        .guides
        .get(guide)
        .map_or(0, |g| g.kind_id)
}

/// `OnScanStateChange(bScanning)` (slot 11): the patrol guide keeps the
/// ship's scan state and scans while undocked; the others do nothing.
pub fn on_scan_state_change(cx: &mut Cx<'_>, guide: GuideRef, scanning: bool) {
    if let Some(g) = cx.logic.content.state.guides.get_mut(guide)
        && let GuideKind::Patrol(p) = &mut g.kind
    {
        p.scan_state = scanning;
        p.scanner.scan_enabled = scanning && p.undocked;
    }
}

/// `GetTargets` (slot 13): the target guide's two targets; none for the
/// goto and dock guides (the base's).
#[must_use]
pub fn targets(cx: &Cx<'_>, guide: GuideRef) -> Targets {
    match cx.logic.content.state.guides.get(guide).map(|g| &g.kind) {
        Some(GuideKind::Nothing { targets, .. }) => *targets,
        Some(GuideKind::Trade(t)) => t.targets,
        Some(GuideKind::Patrol(p)) => p.targets,
        Some(GuideKind::Wander(w)) => w.targets,
        _ => Targets::default(),
    }
}

/// The trade guide's goods (`GetTradeGood`, `GetReturnGood`, slots 19 and
/// 20); none for the other guides.
#[must_use]
pub fn trade_goods(cx: &Cx<'_>, guide: GuideRef) -> Option<(u32, u32)> {
    match cx.logic.content.state.guides.get(guide).map(|g| &g.kind) {
        Some(GuideKind::Trade(t)) => Some((t.trade_good, t.return_good)),
        _ => None,
    }
}

/// The patrol guide's points to go (`GetPathPointsToGo`, slot 19).
#[must_use]
pub fn path_points_to_go(cx: &Cx<'_>, guide: GuideRef) -> Option<i32> {
    match cx.logic.content.state.guides.get(guide).map(|g| &g.kind) {
        Some(GuideKind::Patrol(p)) => Some(p.route.points_to_go(cx)),
        _ => None,
    }
}

/// `RequestFleeDestination` (slot 12) on the guide's members.
fn flee(cx: &Cx<'_>, g: &mut Guide) -> Option<FleeAnswer> {
    let ship = g.ship;
    match &mut g.kind {
        GuideKind::Nothing { targets, .. } => target::flee(cx, ship, targets),
        GuideKind::Wander(w) => target::flee(cx, ship, &mut w.targets),
        GuideKind::Trade(t) => trade::flee(cx, ship, t),
        GuideKind::Patrol(p) => patrol::flee(cx, ship, p),
        GuideKind::Goto(go) | GuideKind::Dock(go, _) => goto::flee(cx, ship, go),
    }
}

/// The read-only part of the guide's `RequestFleeDestination` (slot 12),
/// asked before the tick: the answer on a copy of the guide.
#[must_use]
pub fn flee_answer(cx: &Cx<'_>, guide: GuideRef, ship: NpcShipKey) -> Option<FleeAnswer> {
    let _ = ship;
    let mut copy = cx.logic.content.state.guides.get(guide)?.clone();
    flee(cx, &mut copy)
}

/// The guide's `RequestFleeDestination` with its effects, after the tick:
/// the target guides swap their targets when they take the first, the
/// patrol route turns to its flee state, and the goto guide moves along its
/// path.
pub fn request_flee_destination(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    ship: NpcShipKey,
) -> Option<FleeAnswer> {
    let _ = ship;
    let mut g = cx.logic.content.state.guides.get(guide)?.clone();
    let answer = flee(cx, &mut g);
    if let Some(slot) = cx.logic.content.state.guides.get_mut(guide) {
        *slot = g;
    }
    answer
}

/// `RefreshTarget` (slot 14): the goto and dock guides find their target
/// again (the dock guide its destination too); nothing for the others.
pub fn refresh_target(cx: &mut Cx<'_>, guide: GuideRef) {
    let _ = with_guide(cx, guide, |cx, g| {
        let ship = g.ship;
        match &mut g.kind {
            GuideKind::Goto(go) => goto::refresh_target(cx, ship, go, None),
            GuideKind::Dock(go, dock) => goto::refresh_target(cx, ship, go, Some(dock)),
            _ => {}
        }
    });
}

/// `ForwardToGuide(MsgId, Parms)` (`NpcShip.cpp:954-964`): the guide's
/// `HandleEvent`, then a guide that is done is dropped.
pub fn forward_to_guide(
    cx: &mut Cx<'_>,
    ship: NpcShipKey,
    message: &crate::events::ControllerMessage<'_>,
) {
    let Some(g) = npc_ships::get(cx, ship).and_then(|s| s.guide) else {
        return;
    };
    handle_event(cx, g, message);
    if is_done(cx, g)
        && let Some(old) = npc_ships::get_mut(cx, ship).and_then(|s| s.guide.take())
    {
        release(cx, old);
    }
}

/// `BuildPathDirectives(System, From, To, &Directives, Flags, &Range)`
/// (`SpacePaths.cpp:871-899`): one directive per leg of the NN path, a
/// dock at the lane's rings (`SetTarget(ObjectA, ObjectB)`) or a goto to
/// the free leg's end (`SetDestination(PointB, Range, -1, 0, 0, 0)`), each
/// at priority 1, one reference held by the list.
pub fn build_path_directives(
    cx: &mut Cx<'_>,
    system: u32,
    from: Vector,
    to: Vector,
    range: f32,
) -> DirectiveList {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return Vec::new();
    };
    let (_, legs) = super::space_paths::build_nn_path(record, from, to, range);
    let mut out = Vec::new();
    for leg in legs {
        let kind = match leg.kind {
            0 => {
                let mut dock = Dock {
                    priority: 1,
                    ..Dock::default()
                };
                dock.op.dock_object = leg.object_a;
                dock.op.field_10 = leg.object_b;
                DirectiveKind::Dock(dock)
            }
            1 => {
                let mut goto = super::mission::obj_list::Goto::default();
                set_goto_destination(&mut goto, leg.point_b, range, -1.0, 0, false, false);
                goto.priority = 1;
                DirectiveKind::GotoVec(goto)
            }
            _ => continue,
        };
        out.push(cx.logic.content.state.directives.create(kind));
    }
    out
}

// ---- `Execute` (slot 3) ------------------------------------------------------------------------

/// `DefaultPriority(&Op, Ship)`: a priority of 0 takes the ship's (written
/// into the shared directive).
fn default_priority(priority: &mut u32, ship_priority: i32) {
    if *priority == 0 {
        *priority = ship_priority as u32;
    }
}

/// `MuteNpcShip(Ship)`: an NPC ship that is no SpacePop ship and not muted
/// is muted for 10 s.
fn mute_npc_ship(cx: &mut Cx<'_>, ship: ShipKey) {
    let ShipKey::Npc(key) = ship else {
        return;
    };
    let space_pop = cx.logic.content.state.space_pop;
    let Some(s) = npc_ships::get(cx, key) else {
        return;
    };
    if s.is_space_pop_ship(space_pop) || s.muted {
        return;
    }
    npc_ships::start_timer(cx, key, 10.0);
}

/// `FContentShipBase::SubmitDirective(Op)`: `pub::AI::SubmitDirective(...)
/// == 1`.
fn submit(cx: &mut Cx<'_>, ship: ShipKey, priority: u32, op: DirectiveOp) -> bool {
    let id = ships::id(&cx.logic.content.state, ship);
    let mut base = BaseOp::directive(op);
    base.priority = priority;
    api::ai::submit_directive(cx, id, &base) == api::ai::OpRType::ACCEPTED
}

/// `Execute(Ship)` of a directive (slot 3, `MissionObjList.cpp`): false
/// when it finished at once (the ship then advances). The delay, idle,
/// goto, trail, guide, dock, launch and tractor beam directives; a name to
/// resolve finds nothing (P3.5c, D855), and the kinds only missions give
/// run nothing (P3.5c).
pub fn execute_directive(cx: &mut Cx<'_>, ship: ShipKey, directive: DirectiveRef) -> bool {
    let ship_priority = npc_ships::priority(cx, ship);
    let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
        return false;
    };
    match &mut d.kind {
        DirectiveKind::Delay(delay) => {
            default_priority(&mut delay.priority, ship_priority);
            let (priority, seconds) = (delay.priority, delay.delay_seconds);
            submit(cx, ship, priority, DirectiveOp::Delay(seconds))
        }
        DirectiveKind::Idle(idle) => {
            default_priority(&mut idle.priority, ship_priority);
            let priority = idle.priority;
            submit(cx, ship, priority, DirectiveOp::Idle)
        }
        DirectiveKind::GotoVec(goto)
        | DirectiveKind::GotoSpline(goto)
        | DirectiveKind::GotoShip(goto) => {
            if !goto.target_name.is_empty() || !goto.object_name.is_empty() {
                // `FindTargetId` of a mission's name (P3.5c): a miss
                // finishes the directive.
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            default_priority(&mut goto.priority, ship_priority);
            let (priority, op) = (goto.priority, goto.op.clone());
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Goto(op))
        }
        DirectiveKind::Trail(trail) => {
            // `FObjListTrail::Execute` (`MissionObjList.cpp:540-555`): slot
            // 1 of the ship, whose answer is not read; then the name.
            if !trail.target_name.is_empty() {
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            default_priority(&mut trail.priority, ship_priority);
            let (priority, op) = (trail.priority, trail.op.clone());
            submit(cx, ship, priority, DirectiveOp::Trail(op))
        }
        DirectiveKind::Guide(guide) => {
            // `FObjListGuide::Execute` (`MissionObjList.cpp:640-660`): the
            // follow part follows the goto part's object (written into the
            // shared directive).
            if !guide.target_name.is_empty() {
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            guide.op.follow.target = guide.op.goto.field_5c;
            default_priority(&mut guide.priority, ship_priority);
            let (priority, op) = (guide.priority, guide.op.clone());
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Guide(Box::new(op)))
        }
        DirectiveKind::TractorBeam(tractor) => {
            // `FObjListTractorBeam::Execute` (`:1458-1461`): no priority
            // default.
            let (priority, op) = (tractor.priority, tractor.op.clone());
            submit(cx, ship, priority, DirectiveOp::TractorBeam(op))
        }
        DirectiveKind::Dock(dock) => {
            if !dock.object_name.is_empty() {
                npc_ships::advance_directive(cx, ship);
                return true;
            }
            default_priority(&mut dock.priority, ship_priority);
            let (priority, op) = (dock.priority, dock.op.clone());
            submit(cx, ship, priority, DirectiveOp::Dock(op))
        }
        DirectiveKind::Launch(launch) => {
            if launch.op.index == -1 || launch.op.launch_object == 0 {
                let id = ships::id(&cx.logic.content.state, ship);
                let Some((target, index, terminal)) =
                    super::dock_queues::ship_docking_target(cx, id)
                else {
                    return false;
                };
                let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
                    return false;
                };
                if let DirectiveKind::Launch(launch) = &mut d.kind {
                    launch.op.launch_object = target;
                    launch.op.index = index;
                    launch.op.terminal = terminal as i32;
                }
            }
            let Some(d) = cx.logic.content.state.directives.get_mut(directive) else {
                return false;
            };
            let DirectiveKind::Launch(launch) = &mut d.kind else {
                return false;
            };
            default_priority(&mut launch.priority, ship_priority);
            let (priority, op) = (launch.priority, launch.op.clone());
            let id = ships::id(&cx.logic.content.state, ship);
            super::dock_queues::set_dock_equipment(cx, id, false, false, None);
            submit(cx, ship, priority, DirectiveOp::Launch(op))
        }
        // The other kinds come only from a mission's `[ObjList]` (P3.5c).
        _ => false,
    }
}
