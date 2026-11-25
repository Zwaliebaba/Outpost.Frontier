//! `Execute(Ship)` (slot 3) of the `[ObjList]` directives (TU40,
//! `Content/MissionObjList.cpp:246-1461`; work package P3.5c3, record 0008
//! points 4 and 6): what a ship does when a directive of its queue becomes
//! the active one (`FContentShipBase::AdvanceDirective`,
//! [`npc_ships::advance_directive`]).
//!
//! The guides, the flee answer and the space paths make the delay, idle,
//! goto, trail, guide, dock, launch and tractor beam directives (P3.6c);
//! a mission's `[ObjList]` (`Act_GiveObjList`, the spawns'
//! `init_objectives`) makes every class, and names its targets: a name is
//! resolved when the directive runs (`FindTargetId`, the ship's system
//! record's `FindObjectIdByName` with the ship's controller,
//! [`systems::find_object_id_by_name`]) and written into the shared op, and
//! a name that finds nothing finishes the directive (the ship advances).
//! The directives are shared counted objects ([`crate::content::directives`]),
//! so every write here reaches every holder, as in the C++.
//!
//! The result is the C++'s: false when the directive finished at once
//! without advancing the ship (the caller then advances it), true when it
//! was submitted (the AI's answer) or the ship advanced itself.

use game_sim::ai::behavior::{DirectiveOp, FollowOp};

use super::obj_list::{DirectiveKind, Goto, Zone};
use crate::api;
use crate::api::ai::{BaseOp, Op};
use crate::content::directives::DirectiveRef;
use crate::content::npc_ships;
use crate::content::ships::{self, ShipKey};
use crate::content::{dock_queues, formations, players, systems};
use crate::server::Cx;

/// `GNagIdMask`, the id bits of a space object.
const ID_MASK: u32 = 0x3fff_ffff;

/// `DefaultPriority(&Op, Ship)`: a priority of 0 takes the ship's (written
/// into the shared directive).
fn default_priority(priority: &mut u32, ship_priority: i32) {
    if *priority == 0 {
        *priority = ship_priority as u32;
    }
}

/// `MuteNpcShip(Ship)`: an NPC ship (`Slot04`) that is no SpacePop ship
/// and not muted is muted for 10 s.
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
fn submit_op(cx: &mut Cx<'_>, ship: ShipKey, priority: u32, op: Op) -> bool {
    let id = ships::id(&cx.logic.content.state, ship);
    let base = BaseOp { priority, op };
    api::ai::submit_directive(cx, id, &base) == api::ai::OpRType::ACCEPTED
}

/// [`submit_op`] of a directive op.
fn submit(cx: &mut Cx<'_>, ship: ShipKey, priority: u32, op: DirectiveOp) -> bool {
    submit_op(cx, ship, priority, Op::Directive(op))
}

/// The ship's system record (`GetSystem()`), when Content has it.
fn system_record(cx: &Cx<'_>, ship: ShipKey) -> Option<u32> {
    let state = &cx.logic.content.state;
    ships::system(state, ship).filter(|s| state.systems.contains_key(s))
}

/// `FindTargetId(Ship, &Name)`: 0 without a system record, else the
/// record's `FindObjectIdByName(Name, Ship->ControllerId)`.
fn find_target_id(cx: &Cx<'_>, ship: ShipKey, name: &[u8]) -> u32 {
    let Some(system) = system_record(cx, ship) else {
        return 0;
    };
    let controller = ships::controller(&cx.logic.content.state, ship);
    systems::find_object_id_by_name(cx, system, name, controller)
}

/// The other NPC ships of the ship's system record in its formation (none
/// for a ship in no formation), in the record's order: the formation mates
/// `SubmitStateToFormation` and `FObjListSetLifeTime` reach.
fn formation_mates(cx: &Cx<'_>, ship: ShipKey) -> Vec<crate::events::NpcShipKey> {
    let Some(system) = system_record(cx, ship) else {
        return Vec::new();
    };
    let state = &cx.logic.content.state;
    let own = ships::id(state, ship);
    let formation = ships::formation_id(state, ship);
    let Some(record) = state.systems.get(&system) else {
        return Vec::new();
    };
    record
        .space_objs
        .iter()
        .copied()
        .filter(|k| {
            state.npc_ships.get(k).is_some_and(|o| {
                o.base.ship != own && formation != u32::MAX && o.base.formation_id == formation
            })
        })
        .collect()
}

/// `pub::AI::SubmitState(Ship, Op)` (the result dropped), then the same to
/// every formation mate (`SubmitStateToFormation`).
fn submit_state_to_formation(cx: &mut Cx<'_>, ship: ShipKey, op: &BaseOp) {
    let id = ships::id(&cx.logic.content.state, ship);
    let _ = api::ai::submit_state(cx, id, op);
    for mate in formation_mates(cx, ship) {
        let flid = npc_ships::space_obj(cx, mate);
        let _ = api::ai::submit_state(cx, flid, op);
    }
}

/// `FSystemInfo::FindSpaceObjByNickname` (`SystemInfo.cpp:604-612`): the
/// first NPC ship of the record with the nickname (`strcmp`).
fn find_space_obj_by_nickname(
    cx: &Cx<'_>,
    system: u32,
    name: &[u8],
) -> Option<crate::events::NpcShipKey> {
    let state = &cx.logic.content.state;
    let name = neuron_core::hash::c_str(name);
    state
        .systems
        .get(&system)?
        .space_objs
        .iter()
        .copied()
        .find(|k| {
            state
                .npc_ships
                .get(k)
                .is_some_and(|n| neuron_core::hash::c_str(&n.nickname) == name)
        })
}

/// The resolved names and the priority default written into the shared
/// directive.
fn write(cx: &mut Cx<'_>, directive: DirectiveRef, kind: DirectiveKind) {
    if let Some(d) = cx.logic.content.state.directives.get_mut(directive) {
        d.kind = kind;
    }
}

/// `FObjListGotoShip::Execute` (`:353-382`) and `ExecuteGotoToObject` of
/// `GotoSpline` and `GotoVec` (`:325-350`): `GotoShip`'s target name into
/// `Field_1C` (the other two have none), the object name into `Field_5C`.
fn run_goto(
    cx: &mut Cx<'_>,
    ship: ShipKey,
    directive: DirectiveRef,
    mut goto: Goto,
    rebuild: fn(Goto) -> DirectiveKind,
) -> bool {
    if !goto.target_name.is_empty() {
        let id = find_target_id(cx, ship, &goto.target_name);
        goto.op.field_1c = id.cast_signed();
        if id & ID_MASK == 0 {
            write(cx, directive, rebuild(goto));
            return advance(cx, ship);
        }
    }
    if !goto.object_name.is_empty() {
        let id = find_target_id(cx, ship, &goto.object_name);
        goto.op.field_5c = id;
        if id & ID_MASK == 0 {
            write(cx, directive, rebuild(goto));
            return advance(cx, ship);
        }
    }
    default_priority(&mut goto.priority, npc_ships::priority(cx, ship));
    let (priority, op) = (goto.priority, goto.op.clone());
    write(cx, directive, rebuild(goto));
    mute_npc_ship(cx, ship);
    submit(cx, ship, priority, DirectiveOp::Goto(op))
}

/// `ExecuteZone` of `StayInRange` and `StayOutOfRange` (`:946-975`): the
/// object name into the zone's object, then `SubmitState` to the ship and
/// its formation mates; the ship advances. A box's extent is computed and
/// not used.
fn run_zone(
    cx: &mut Cx<'_>,
    ship: ShipKey,
    directive: DirectiveRef,
    mut zone: Zone,
    rebuild: fn(Zone) -> DirectiveKind,
) -> bool {
    if !zone.object_name.is_empty() {
        let id = find_target_id(cx, ship, &zone.object_name);
        zone.op.target_id = id;
        if id & ID_MASK == 0 {
            write(cx, directive, rebuild(zone));
            return advance(cx, ship);
        }
    }
    default_priority(&mut zone.priority, npc_ships::priority(cx, ship));
    let op = BaseOp {
        priority: zone.priority,
        op: Op::SetZoneBehaviorParams(zone.op.clone()),
    };
    write(cx, directive, rebuild(zone));
    submit_state_to_formation(cx, ship, &op);
    advance(cx, ship)
}

/// The ship advances its queue, and the directive counts as run.
fn advance(cx: &mut Cx<'_>, ship: ShipKey) -> bool {
    npc_ships::advance_directive(cx, ship);
    true
}

/// `Execute(Ship)` of `directive` (slot 3, `MissionObjList.cpp`) on the
/// ship whose active directive it became.
#[allow(clippy::too_many_lines)]
pub fn execute(cx: &mut Cx<'_>, ship: ShipKey, directive: DirectiveRef) -> bool {
    let ship_priority = npc_ships::priority(cx, ship);
    let Some(kind) = cx
        .logic
        .content
        .state
        .directives
        .get(directive)
        .map(|d| d.kind.clone())
    else {
        return false;
    };
    match kind {
        // `FObjListAvoidance::Execute` (`:246-252`).
        DirectiveKind::Avoidance(a) => {
            let op = BaseOp {
                priority: a.priority,
                op: Op::SetFlagParams(a.op),
            };
            submit_state_to_formation(cx, ship, &op);
            advance(cx, ship)
        }
        // `FObjListDelay::Execute` and `FObjListIdle::Execute` (`:279-308`).
        DirectiveKind::Delay(mut delay) => {
            default_priority(&mut delay.priority, ship_priority);
            let (priority, seconds) = (delay.priority, delay.delay_seconds);
            write(cx, directive, DirectiveKind::Delay(delay));
            submit(cx, ship, priority, DirectiveOp::Delay(seconds))
        }
        DirectiveKind::Idle(mut idle) => {
            default_priority(&mut idle.priority, ship_priority);
            let priority = idle.priority;
            write(cx, directive, DirectiveKind::Idle(idle));
            submit(cx, ship, priority, DirectiveOp::Idle)
        }
        // `FObjListGotoShip::Execute` (`:353-382`) and `ExecuteGotoToObject`
        // of `GotoSpline` and `GotoVec` (`:325-350`): `GotoShip`'s target
        // into `Field_1C`, the object name into `Field_5C`.
        DirectiveKind::GotoShip(goto) => {
            run_goto(cx, ship, directive, goto, DirectiveKind::GotoShip)
        }
        DirectiveKind::GotoSpline(goto) => {
            run_goto(cx, ship, directive, goto, DirectiveKind::GotoSpline)
        }
        DirectiveKind::GotoVec(goto) => run_goto(cx, ship, directive, goto, DirectiveKind::GotoVec),
        // `FObjListTrail::Execute` (`:540-555`).
        DirectiveKind::Trail(mut trail) => {
            if !trail.target_name.is_empty() {
                let id = find_target_id(cx, ship, &trail.target_name);
                trail.op.target = id;
                if id & ID_MASK == 0 {
                    write(cx, directive, DirectiveKind::Trail(trail));
                    return advance(cx, ship);
                }
            }
            default_priority(&mut trail.priority, ship_priority);
            let (priority, op) = (trail.priority, trail.op.clone());
            write(cx, directive, DirectiveKind::Trail(trail));
            submit(cx, ship, priority, DirectiveOp::Trail(op))
        }
        // `FObjListFollow::Execute` (`:585-600`).
        DirectiveKind::Follow(mut follow) => {
            if !follow.target_name.is_empty() {
                let id = find_target_id(cx, ship, &follow.target_name);
                follow.op.target = id;
                if id & ID_MASK == 0 {
                    write(cx, directive, DirectiveKind::Follow(follow));
                    return advance(cx, ship);
                }
            }
            default_priority(&mut follow.priority, ship_priority);
            let (priority, op) = (follow.priority, follow.op.clone());
            write(cx, directive, DirectiveKind::Follow(follow));
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Follow(op))
        }
        // `FObjListGuide::Execute` (`:640-660`): the follow part follows the
        // goto part's object.
        DirectiveKind::Guide(mut guide) => {
            if !guide.target_name.is_empty() {
                let id = find_target_id(cx, ship, &guide.target_name);
                guide.op.goto.field_5c = id;
                if id & ID_MASK == 0 {
                    write(cx, directive, DirectiveKind::Guide(guide));
                    return advance(cx, ship);
                }
            }
            guide.op.follow.target = guide.op.goto.field_5c;
            default_priority(&mut guide.priority, ship_priority);
            let (priority, op) = (guide.priority, guide.op.clone());
            write(cx, directive, DirectiveKind::Guide(guide));
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Guide(Box::new(op)))
        }
        // `FObjListGuideShip::Execute` (`:734-767`): both names are resolved
        // without a length test; the escorted object's location is asked
        // and not used.
        DirectiveKind::GuideShip(mut guide) => {
            let target = find_target_id(cx, ship, &guide.target_name);
            guide.op.goto.field_5c = target;
            if target & ID_MASK == 0 {
                write(cx, directive, DirectiveKind::GuideShip(guide));
                return advance(cx, ship);
            }
            let object = find_target_id(cx, ship, &guide.object_name);
            guide.op.goto.field_1c = object.cast_signed();
            if object & ID_MASK == 0 {
                write(cx, directive, DirectiveKind::GuideShip(guide));
                return advance(cx, ship);
            }
            guide.op.follow.target = guide.op.goto.field_5c;
            default_priority(&mut guide.priority, ship_priority);
            let (priority, op) = (guide.priority, guide.op.clone());
            write(cx, directive, DirectiveKind::GuideShip(guide));
            let _ = api::space_obj::location(cx, op.goto.field_1c.cast_unsigned());
            mute_npc_ship(cx, ship);
            submit(cx, ship, priority, DirectiveOp::Guide(Box::new(op)))
        }
        // `FObjListFace::Execute` (`:812-827`): only mode 1 faces a named
        // object.
        DirectiveKind::Face(mut face) => {
            if face.op.mode == 1 && !face.target_name.is_empty() {
                let id = find_target_id(cx, ship, &face.target_name);
                face.op.target = id;
                if id & ID_MASK == 0 {
                    write(cx, directive, DirectiveKind::Face(face));
                    return advance(cx, ship);
                }
            }
            default_priority(&mut face.priority, ship_priority);
            let (priority, op) = (face.priority, face.op.clone());
            write(cx, directive, DirectiveKind::Face(face));
            submit(cx, ship, priority, DirectiveOp::Face(op))
        }
        // `FObjListSetLifeTime::Execute` (`:867-897`): an NPC ship and its
        // formation mates take the life (`+0x1bc` and `+0x1c8`).
        DirectiveKind::SetLifeTime(set) => {
            let life = set.life_time as f32;
            let mut ships_to_set = formation_mates(cx, ship);
            if let ShipKey::Npc(key) = ship {
                ships_to_set.insert(0, key);
            }
            for key in ships_to_set {
                if let Some(n) = npc_ships::get_mut(cx, key) {
                    n.life_time = life;
                    n.life_left = life;
                }
            }
            advance(cx, ship)
        }
        // `FObjListSetPriority::Execute` (`:910-915`).
        DirectiveKind::SetPriority(set) => {
            npc_ships::set_priority(cx, ship, set.priority);
            advance(cx, ship)
        }
        // `ExecuteZone` of `StayInRange` and `StayOutOfRange` (`:946-975`):
        // a box's extent is computed and not used.
        DirectiveKind::StayInRange(zone) => {
            run_zone(cx, ship, directive, zone, DirectiveKind::StayInRange)
        }
        DirectiveKind::StayOutOfRange(zone) => {
            run_zone(cx, ship, directive, zone, DirectiveKind::StayOutOfRange)
        }
        // `FObjListMakeNewFormation::Execute` (`:1077-1105`): the ship and
        // the named NPC ships of its system (each cancelling its directives)
        // form the directive's formation.
        DirectiveKind::MakeNewFormation(f) => {
            if let Some(system) = system_record(cx, ship) {
                let mut members = vec![ship];
                let own = ships::id(&cx.logic.content.state, ship);
                for name in &f.member_names {
                    let Some(key) = find_space_obj_by_nickname(cx, system, name) else {
                        continue;
                    };
                    if npc_ships::space_obj(cx, key) == own {
                        continue;
                    }
                    npc_ships::cancel_directives(cx, ShipKey::Npc(key));
                    members.push(ShipKey::Npc(key));
                }
                formations::form_formation(cx, &mut members, f.formation, f.add_followers);
            }
            advance(cx, ship)
        }
        // `FObjListFollowPlayer::Execute` (`:1147-1189`): each named NPC ship
        // of the ship's system leaves its formation and follows the ship at
        // its slot's offset from the leader's slot (the formation's last
        // slot when that is a player slot, else none: the zero vector).
        DirectiveKind::FollowPlayer(f) => {
            let mut result = true;
            let count = formations::slot_count(cx, f.formation);
            let leader_slot = if formations::is_player_slot(cx, f.formation, count - 1) {
                count - 1
            } else {
                -1
            };
            let own = ships::id(&cx.logic.content.state, ship);
            let mut member_slot = 0_u32;
            // The C++ reads the system record without a test (D990).
            if let Some(system) = system_record(cx, ship) {
                for name in &f.member_names {
                    let Some(key) = find_space_obj_by_nickname(cx, system, name) else {
                        continue;
                    };
                    if npc_ships::space_obj(cx, key) == own {
                        continue;
                    }
                    let member = ShipKey::Npc(key);
                    let state = &cx.logic.content.state;
                    if ships::formation_slot(state, member) != 0
                        && ships::formation_id(state, member) != u32::MAX
                    {
                        formations::remove_from_formation(cx, member);
                    }
                    let leader =
                        formations::slot_position(cx, f.formation, leader_slot.cast_unsigned());
                    let at = formations::slot_position(cx, f.formation, member_slot);
                    member_slot += 1;
                    let mut op = FollowOp {
                        target: own,
                        distance: 1000.0,
                        ..FollowOp::default()
                    };
                    op.offset.x = (f64::from(at.x) - f64::from(leader.x)) as f32;
                    op.offset.y = (f64::from(at.y) - f64::from(leader.y)) as f32;
                    op.offset.z = (f64::from(at.z) - f64::from(leader.z)) as f32;
                    npc_ships::cancel_directives(cx, member);
                    let mut priority = 0_u32;
                    default_priority(&mut priority, npc_ships::priority(cx, member));
                    result = submit(cx, member, priority, DirectiveOp::Follow(op));
                }
            }
            npc_ships::advance_directive(cx, ship);
            result
        }
        // `FObjListBreakFormation::Execute` (`:1217-1222`).
        DirectiveKind::BreakFormation => {
            formations::remove_from_formation(cx, ship);
            advance(cx, ship)
        }
        // `FObjListCancelObjective::Execute` (`:1232-1240`).
        DirectiveKind::CancelObjective => {
            if ships::formation_id(&cx.logic.content.state, ship) == u32::MAX {
                npc_ships::cancel_directives(cx, ship);
            } else {
                formations::remove_from_formation(cx, ship);
            }
            advance(cx, ship)
        }
        // `FObjListDock::Execute` (`:1260-1273`): the object name into
        // `Field_1C`.
        DirectiveKind::Dock(mut dock) => {
            if !dock.object_name.is_empty() {
                let id = find_target_id(cx, ship, &dock.object_name);
                dock.op.field_1c = id;
                if id & ID_MASK == 0 {
                    write(cx, directive, DirectiveKind::Dock(dock));
                    return advance(cx, ship);
                }
            }
            default_priority(&mut dock.priority, ship_priority);
            let (priority, op) = (dock.priority, dock.op.clone());
            write(cx, directive, DirectiveKind::Dock(dock));
            submit(cx, ship, priority, DirectiveOp::Dock(op))
        }
        // `FObjListInstantTradelane::Execute` (`:1313-1318`).
        DirectiveKind::InstantTradelane(mut lane) => {
            default_priority(&mut lane.priority, ship_priority);
            let (priority, op) = (lane.priority, lane.op.clone());
            write(cx, directive, DirectiveKind::InstantTradelane(lane));
            submit(cx, ship, priority, DirectiveOp::InstantTradeLane(op))
        }
        // `FObjListLaunch::Execute` (`:1351-1364`): without a launch object
        // or index the ship's docking target, else nothing to launch from.
        DirectiveKind::Launch(mut launch) => {
            if launch.op.index == -1 || launch.op.launch_object == 0 {
                let id = ships::id(&cx.logic.content.state, ship);
                let Some((target, index, terminal)) = dock_queues::ship_docking_target(cx, id)
                else {
                    return false;
                };
                launch.op.launch_object = target;
                launch.op.index = index;
                launch.op.terminal = terminal as i32;
            }
            default_priority(&mut launch.priority, ship_priority);
            let (priority, op) = (launch.priority, launch.op.clone());
            write(cx, directive, DirectiveKind::Launch(launch));
            let id = ships::id(&cx.logic.content.state, ship);
            dock_queues::set_dock_equipment(cx, id, false, false, None);
            submit(cx, ship, priority, DirectiveOp::Launch(op))
        }
        // `FObjListWaitForPlayerManeuver::Execute` (`:1422-1442`): no player
        // ship stays on the directive; a player in behaviour 10 whose ship
        // flies in the ship's formation finishes it.
        DirectiveKind::WaitForPlayerManeuver(mut wait) => {
            let player = find_target_id(cx, ship, b"Player");
            if player == 0 {
                return true;
            }
            if api::ai::behavior_id(cx, player) == 10 {
                let state = &cx.logic.content.state;
                // The C++ reads the player's state without a test (D990).
                let same = players::find_by_ship(&state.players, player)
                    .and_then(|p| state.players.get(&p))
                    .and_then(|p| p.ship.as_ref())
                    .is_some_and(|s| s.formation_id == ships::formation_id(state, ship));
                if same {
                    return advance(cx, ship);
                }
            }
            default_priority(&mut wait.priority, ship_priority);
            let (priority, name) = (wait.priority, wait.behavior_name.clone());
            write(cx, directive, DirectiveKind::WaitForPlayerManeuver(wait));
            submit_op(cx, ship, priority, Op::WaitForPlayerManeuver(name))
        }
        // `FObjListTractorBeam::Execute` (`:1458-1461`): no priority default.
        DirectiveKind::TractorBeam(tractor) => {
            let (priority, op) = (tractor.priority, tractor.op.clone());
            submit(cx, ship, priority, DirectiveOp::TractorBeam(op))
        }
    }
}
