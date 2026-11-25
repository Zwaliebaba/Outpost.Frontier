//! Content's NPC ships through the server (work package P3.6c, decision
//! record 0008 points 4 and 7) over the original data (`outdir/`, he01):
//! the oracle's start-up (Content's GameSupport, SpaceSupport and
//! BaseSupport, then `LoadGame` over `InitialWorld.ini`), he01 loaded, and
//! an NPC ship made by `FSystemInfo::CreateNpcShip` with a "nothing" guide.
//! Expected values follow the C++ (`Content/NpcShip.cpp`, `Guides.cpp`).
//!
//! - The ship is its manager's directive and content callback: one handle
//!   in both, the creation type its kind maps to.
//! - The guide starts at once: its 10000 s delay (priority 1) is the
//!   ship's one directive, running.
//! - The flee answers: a position (state 1), an object (2), a dock (3),
//!   and none from a guide without targets.
//! - The heartbeat runs the ship's life down; a destroyed ship leaves the
//!   record, the arena and the callbacks, and its guide is released.
//! - The trade guide plans the NN path to its second target; the patrol
//!   guide flies its zone path with its cargo scanner and answers the flee
//!   toward its next point; the wander guide flies around the ship; the
//!   goto and dock guides of a `[MetaBehavior]` find their target by name
//!   and lead it with guide directives (P3.6c's follow-up).

use std::path::Path;
use std::sync::Arc;

use game_logic::api::{reputation, space_obj};
use game_logic::content::callbacks::{self, CallbackOwner};
use game_logic::content::directives::DirectiveRef;
use game_logic::content::guides::{
    self, Guide, GuideDesc, GuideKind, GuideRef, PatrolPathRef, SpawnPlan,
};
use game_logic::content::mission::meta_behavior;
use game_logic::content::mission::obj_list::DirectiveKind;
use game_logic::content::npc_ships::{self, NpcShipParams};
use game_logic::content::rep_handles::{RepHandle, RepKind};
use game_logic::content::ships::ShipKey;
use game_logic::events::{ControllerMessage, NpcShipKey};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::ai::content::FleeAnswer;
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// he01 on the oracle's start-up, `None` without `outdir/`.
fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no NPC ship was made");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 4, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = controller::create(&mut cx, b"Content.dll", name, &CreateParams::None, priority);
        assert_ne!(id.0, 0);
    }
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    game_logic::selection::load_world_state(&mut cx, &mut reader);
    world::load(&mut cx, create_id(b"he01"));
    Some((world, logic))
}

fn fighter_params(cx: &Cx<'_>) -> NpcShipParams {
    NpcShipParams {
        controller: cx.logic.content.state.space_support.0,
        archetype: create_id(b"he_fighter"),
        loadout: create_id(b"li_p_he_fighter_loadout01"),
        position: Vector::new(15000.0, 3000.0, -25000.0),
        state_graph: cx
            .world
            .data()
            .state_graphs
            .find(b"FIGHTER", GraphType::Leader),
        guide: Some(GuideDesc {
            kind: 1,
            ..GuideDesc::default()
        }),
        ..NpcShipParams::default()
    }
}

#[test]
fn an_npc_ship_is_its_managers_callbacks_and_runs_its_guide() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let params = fighter_params(&cx);
    let next_handle = cx.logic.content.state.callbacks.peek_next();
    let (result, key) = npc_ships::create_in_system(&mut cx, he01, &params, 0);
    assert_eq!(result, 0);
    let key = key.expect("the ship is made");
    let ship = npc_ships::get(&cx, key).expect("in the arena").clone();
    assert_eq!(ship.callback, next_handle);
    assert_eq!(
        cx.logic.content.state.callbacks.owner(ship.callback),
        Some(CallbackOwner::NpcShip(key))
    );
    assert_eq!(cx.logic.content.state.systems[&he01].space_objs, vec![key]);
    let h = cx.world.find(ship.base.ship).expect("the space object");
    let m = cx.world.behavior_manager(h).expect("a manager");
    assert_eq!(m.state.content_handle, ship.callback);
    // Creation kind 2 answers creation type 0.
    assert_eq!(m.state.creation_type, 0);
    assert!(m.state.directive_callbacks.contains(&ship.callback));
    // The nothing guide's delay is the one directive, running.
    let q = npc_ships::queue(&cx, ShipKey::Npc(key)).unwrap();
    assert_eq!(q.list.len(), 1);
    assert_eq!(q.current, Some(0));
    assert_eq!(q.active, Some(q.list[0]));
    let guide = ship.guide.expect("a guide");
    assert_eq!(game_logic::content::guides::guide_type(&cx, guide), 1);

    // The flee answers.
    assert_eq!(callbacks::flee_answer(&cx, ship.callback), None);
    npc_ships::set_flee_position(&mut cx, key, Vector::new(1.0, 2.0, 3.0), 700.0);
    assert_eq!(
        callbacks::flee_answer(&cx, ship.callback),
        Some(FleeAnswer {
            kind: 3,
            position: Vector::new(1.0, 2.0, 3.0),
            radius: 700.0,
            ..FleeAnswer::default()
        })
    );
    npc_ships::set_flee_object(&mut cx, key, 77, 300.0, true);
    assert_eq!(
        callbacks::flee_answer(&cx, ship.callback).map(|a| (a.kind, a.param, a.radius)),
        Some((2, 77, 300.0))
    );
    npc_ships::clear_flee(&mut cx, key);

    // The ship's life is -1 (immortal): the heartbeat leaves it.
    let to = cx.logic.content.state.space_support;
    let _ = cx.dispatch(
        to,
        &mut game_logic::events::ControllerMessage::Heartbeat { elapsed: 1.0 },
    );
    assert!(npc_ships::get(&cx, key).is_some());

    // Destroyed: the record, the arena, the callbacks and the guide forget
    // it.
    let _ = game_logic::api::space_obj::destroy(&mut cx, ship.base.ship, 1);
    let _ = world::step_system(&mut cx, he01, 1.0 / 30.0);
    assert!(npc_ships::get(&cx, key).is_none());
    assert!(cx.logic.content.state.systems[&he01].space_objs.is_empty());
    assert_eq!(cx.logic.content.state.callbacks.owner(ship.callback), None);
    assert!(cx.logic.content.state.guides.get(guide).is_none());
}

#[test]
fn a_ship_queued_at_a_station_launches_from_a_berth() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let station = cx
        .world
        .get(cx.world.find(create_id(b"he01_11")).expect("he01_11"))
        .unwrap()
        .simple
        .id;
    let mut params = fighter_params(&cx);
    params.class_ids = vec![create_id(b"can_use_berths")];
    params.guide = None;
    let (result, key) = npc_ships::create_in_system(&mut cx, he01, &params, station);
    // Made at once: the queue is empty again (`operator[]` leaves it in the
    // map for the next pass to drop), so the block counts as not queued
    // (0), and a ship whose life is -1 is not the pass's result.
    assert_eq!(result, 0);
    assert_eq!(key, None);
    assert!(cx.logic.content.state.dock_queues.launch[&station].is_empty());
    let ships = cx.logic.content.state.systems[&he01].space_objs.clone();
    assert_eq!(ships.len(), 1);
    let ship = npc_ships::get(&cx, ships[0]).unwrap().clone();
    // The launch directive leads, running; the ship is in the docking list
    // as launched from the station.
    let q = npc_ships::queue(&cx, ShipKey::Npc(ships[0])).unwrap();
    assert_eq!(q.list.len(), 1);
    let first = cx.logic.content.state.directives.get(q.list[0]).unwrap();
    assert_eq!(first.kind.type_code(), 0x15);
    let entry =
        game_logic::content::dock_queues::docking_entry_of(&cx.logic.content.state, ship.base.ship)
            .expect("a docking entry");
    assert_eq!(entry.target, station);
    assert!(entry.inside);
}

// ---- the guides of types 2 to 6 (P3.6c's follow-up) ----------------------------------------------

/// The space object id of a solar of he01.
fn solar_id(cx: &Cx<'_>, nickname: &[u8]) -> u32 {
    let h = cx.world.find(create_id(nickname)).expect("the solar");
    cx.world.get(h).unwrap().simple.id
}

/// A reputation handle around a fresh vibe of `group` (the C++'s
/// spawners give every ship one).
fn faction_handle(cx: &mut Cx<'_>, group: &[u8]) -> RepHandle {
    let vibe = reputation::alloc(cx, &FmtStr::new(0), &FmtStr::new(0)).expect("a vibe");
    let faction = reputation::reputation_group(cx, group).expect("the faction");
    reputation::set_affiliation(cx, vibe, faction).expect("its affiliation");
    cx.logic
        .content
        .state
        .rep_handles
        .create(vibe, RepKind::Plain)
}

/// A he_p_grp fighter of SpaceSupport's with `desc` as its guide.
fn guided(cx: &mut Cx<'_>, desc: GuideDesc, at: Vector) -> NpcShipKey {
    let mut params = fighter_params(cx);
    params.reputation = Some(faction_handle(cx, b"he_p_grp"));
    params.position = at;
    params.guide = Some(desc);
    let (_, key) = npc_ships::create_in_system(cx, create_id(b"he01"), &params, 0);
    npc_ships::release_params(cx, params);
    key.expect("the ship is made")
}

fn guide_of(cx: &Cx<'_>, ship: NpcShipKey) -> (GuideRef, Guide) {
    let g = npc_ships::get(cx, ship).unwrap().guide.expect("a guide");
    (g, cx.logic.content.state.guides.get(g).unwrap().clone())
}

fn directive(cx: &Cx<'_>, d: DirectiveRef) -> DirectiveKind {
    cx.logic
        .content
        .state
        .directives
        .get(d)
        .unwrap()
        .kind
        .clone()
}

fn ship_queue(cx: &Cx<'_>, ship: NpcShipKey) -> Vec<DirectiveRef> {
    npc_ships::queue(cx, ShipKey::Npc(ship))
        .unwrap()
        .list
        .clone()
}

fn space_support(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    let to = cx.logic.content.state.space_support;
    let _ = cx.dispatch(to, message);
}

#[test]
fn a_trade_guide_plans_the_way_to_its_second_target() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (from, to) = (solar_id(&cx, b"he01_11"), solar_id(&cx, b"he01_03"));
    let mut desc = GuideDesc {
        kind: 2,
        trade_good: 11,
        return_good: 22,
        ..GuideDesc::default()
    };
    // A station's base is its docking target's record.
    desc.set_targets(&mut cx, from, to, -1);
    assert_eq!(desc.base1, Some(create_id(b"he01_11_Base")));
    assert_eq!(desc.base2, Some(create_id(b"he01_03_base")));
    let ship = guided(&mut cx, desc, Vector::new(15000.0, 3000.0, -25000.0));
    let (g, guide) = guide_of(&cx, ship);
    assert_eq!(guides::guide_type(&cx, g), 2);
    assert_eq!(guides::trade_goods(&cx, g), Some((11, 22)));
    let GuideKind::Trade(trade) = &guide.kind else {
        panic!("a trade guide");
    };
    // The route aims at the second target's centre, within its radius plus
    // 3000, and gave the ship the NN path's directives (priority 1).
    let (radius, center) = space_obj::radius(&cx, to).unwrap();
    assert_eq!(trade.route.center, center);
    assert_eq!(trade.route.range, radius + 3000.0);
    assert_eq!(trade.state, 0);
    let queue = ship_queue(&cx, ship);
    assert!(!queue.is_empty());
    match directive(&cx, queue[0]) {
        DirectiveKind::GotoVec(goto) => {
            assert_eq!(trade.route.state, 1);
            assert_eq!(goto.priority, 1);
            assert_eq!(goto.op.range, trade.route.range);
        }
        DirectiveKind::Dock(dock) => {
            assert_eq!(trade.route.state, 2);
            assert_eq!(dock.priority, 1);
        }
        other => panic!("{other:?}"),
    }
    // The flee: a dock at the target (type 2), or toward a lane's first
    // ring (2 within 10000, else 3 with radius 500).
    let h = npc_ships::get(&cx, ship).unwrap().callback;
    let answer = callbacks::flee_answer(&cx, h).expect("an answer");
    assert!(
        (answer.kind == 2 && answer.param != 0) || (answer.kind == 3 && answer.radius == 500.0),
        "{answer:?}"
    );
    // The answer has no effects for a trade guide.
    assert_eq!(cx.logic.content.state.guides.get(g).unwrap(), &guide);
}

#[test]
fn a_patrol_guide_flies_its_path_and_scans() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let record = &cx.logic.content.state.systems[&he01];
    let Some(index) = record.paths.iter().position(|p| p.points.len() >= 3) else {
        panic!("he01 has patrol paths");
    };
    let path = record.paths[index].clone();
    let last = path.points.len() as i32 - 1;
    let desc = GuideDesc {
        kind: 3,
        target1: solar_id(&cx, b"he01_11"),
        target2: solar_id(&cx, b"he01_03"),
        plan: SpawnPlan {
            path: Some(PatrolPathRef {
                system: he01,
                index,
            }),
            path_target_point: 1,
            path_point: last,
            path_direction: 1,
            ..SpawnPlan::default()
        },
        ..GuideDesc::default()
    };
    let ship = guided(&mut cx, desc, path.points[0]);
    let (g, guide) = guide_of(&cx, ship);
    let GuideKind::Patrol(patrol) = &guide.kind else {
        panic!("a patrol guide");
    };
    // The ship joined the guided ships; the route flies to point 1.
    assert!(cx.logic.content.state.guided_ships.contains(&Some(ship)));
    assert_eq!((patrol.state, patrol.route.state), (1, 2));
    assert_eq!(patrol.route.points_to_go(&cx), last - 1);
    let queue = ship_queue(&cx, ship);
    assert_eq!(queue.len(), 1);
    let DirectiveKind::GotoVec(goto) = directive(&cx, queue[0]) else {
        panic!("a point goto");
    };
    assert_eq!(goto.op.destination, path.points[1]);
    assert_eq!(
        (goto.op.range, goto.op.field_58, goto.priority),
        (500.0, 1, 1)
    );
    // The scanner: he_p_grp scans for cargo, announces it, at its 0.3
    // chance (the pilot has none of its own); on when the ship's scanner
    // reaches beyond 500.
    let id = npc_ships::space_obj(&cx, ship);
    let range = space_obj::scanner_range(&cx, id).map_or(0, |(_, s)| s);
    let s = &patrol.scanner;
    if range > 500 {
        assert_eq!((s.state, s.scan_range), (1, 500));
        assert!(s.scan_announce);
        assert_eq!(s.scan_chance, 0.3_f32);
    } else {
        assert_eq!(s.state, 0);
    }
    // The flee: toward the next point (type 3, radius 500), and the route
    // turns to its flee state only with the answer's effects.
    let h = npc_ships::get(&cx, ship).unwrap().callback;
    let answer = callbacks::flee_answer(&cx, h).expect("an answer");
    assert_eq!((answer.kind, answer.radius), (3, 500.0));
    let after = guides::request_flee_destination(&mut cx, g, ship).unwrap();
    assert_eq!(after, answer);
    let GuideKind::Patrol(patrol) = &cx.logic.content.state.guides.get(g).unwrap().kind else {
        panic!();
    };
    assert_eq!(patrol.route.state, 3);
    // A clone copies the reference count and joins no guided ships; as in
    // the C++, its destructor unregisters the ship it shares with the
    // original (`UnregisterGuidedShip(Ship)`).
    let guided_before = cx.logic.content.state.guided_ships.len();
    let copy = guides::clone_guide(&mut cx, g).unwrap();
    assert_eq!(cx.logic.content.state.guides.refs(copy), 1);
    assert_eq!(cx.logic.content.state.guided_ships.len(), guided_before);
    guides::release(&mut cx, copy);
    assert!(cx.logic.content.state.guides.get(copy).is_none());
    assert!(!cx.logic.content.state.guided_ships.contains(&Some(ship)));
    // The ship's destruction releases the guide.
    let _ = space_obj::destroy(&mut cx, id, 1);
    let _ = world::step_system(&mut cx, he01, 1.0 / 30.0);
    assert!(cx.logic.content.state.guides.get(g).is_none());
}

#[test]
fn a_wander_guide_flies_around_the_ship() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let at = Vector::new(15000.0, 3000.0, -25000.0);
    let desc = GuideDesc {
        kind: 4,
        ..GuideDesc::default()
    };
    let ship = guided(&mut cx, desc, at);
    let (_, guide) = guide_of(&cx, ship);
    let GuideKind::Wander(wander) = &guide.kind else {
        panic!("a wander guide");
    };
    assert_eq!(wander.state, 1);
    let held = wander.directive.expect("the directive it handed out");
    // The guide and the ship's queue hold it.
    assert_eq!(ship_queue(&cx, ship), vec![held]);
    assert_eq!(cx.logic.content.state.directives.refs(held), 3);
    match directive(&cx, held) {
        DirectiveKind::GotoVec(goto) => {
            // A point 1500 to 3000 from (x, 500, z) of the ship, range 500.
            let d = goto.op.destination;
            assert_eq!(d.y, 500.0);
            let (dx, dz) = (f64::from(d.x - at.x), f64::from(d.z - at.z));
            let r = (dx * dx + dz * dz).sqrt();
            assert!((1499.0..=3001.0).contains(&r), "{d:?}");
            assert_eq!(
                (goto.op.range, goto.op.cruise, goto.op.no_cruise),
                (500.0, 0, 1)
            );
            assert_eq!((wander.min_range, wander.max_range), (1500.0, 3000.0));
        }
        DirectiveKind::Delay(delay) => {
            assert_eq!(delay.delay_seconds, 0.5);
            assert_eq!(wander.min_range, 750.0);
        }
        other => panic!("{other:?}"),
    }
}

#[test]
fn the_goto_and_dock_guides_lead_their_target() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    // The escorted ship, found by its name.
    let mut params = fighter_params(&cx);
    params.nickname = b"escortee".to_vec();
    params.position = Vector::new(15200.0, 3000.0, -25000.0);
    params.guide = None;
    let (_, escortee) = npc_ships::create_in_system(&mut cx, he01, &params, 0);
    let escortee_id = npc_ships::space_obj(&cx, escortee.unwrap());

    let text = "[MetaBehavior]\n\
        MB_GotoGuide = escortee, 0, 0, 150, -20000, 0, 20000, 750, true, NORMAL\n\
        MB_DockGuide = escortee, 0, 0, 150, he01_03, 600\n";
    let mut reader = IniReader::new();
    reader.open_memory(text.as_bytes());
    assert!(reader.read_header());
    let mut nickname = 0;
    let mut list = meta_behavior::load(&mut reader, &mut nickname);
    let dock_meta = list.pop().unwrap();
    let goto_meta = list.pop().unwrap();

    // The goto guide: its target by name, the NN path to the destination,
    // among the guided ships.
    let at = Vector::new(15000.0, 3000.0, -25000.0);
    let ship = guided(&mut cx, GuideDesc::from_meta(goto_meta), at);
    let (g, guide) = guide_of(&cx, ship);
    assert_eq!(guides::guide_type(&cx, g), 5);
    let GuideKind::Goto(goto) = &guide.kind else {
        panic!("a goto guide");
    };
    assert_eq!(goto.target_id, escortee_id);
    assert_eq!(goto.state, 1);
    assert!(!goto.path.is_empty());
    let first = goto.path[0];
    assert!(cx.logic.content.state.guided_ships.contains(&Some(ship)));
    // The first heartbeat starts it: state 2 and a guide directive to the
    // first leg's start with the follow offset and the line's priority.
    space_support(&mut cx, &mut ControllerMessage::Heartbeat { elapsed: 0.1 });
    let GuideKind::Goto(goto) = &cx.logic.content.state.guides.get(g).unwrap().kind else {
        panic!();
    };
    assert_eq!((goto.state, goto.prev_state, goto.restart), (2, 1, false));
    let held = goto.directive.expect("its guide directive");
    let DirectiveKind::Guide(d) = directive(&cx, held) else {
        panic!("a guide directive");
    };
    assert_eq!(d.priority, 1);
    assert_eq!(d.op.goto.destination, first.point_a);
    assert_eq!(
        (d.op.goto.field_5c, d.op.follow.target),
        (escortee_id, escortee_id)
    );
    assert_eq!(d.op.follow.offset, Vector::new(0.0, 0.0, 150.0));
    assert_eq!(
        (d.op.goto.range, d.op.goto.field_60, d.op.goto.field_64),
        (500.0, 200.0, 500.0)
    );
    assert_eq!(d.op.field_a0, 1);
    // A goto guide's flee answer moves along its path: state 2 was entered
    // from 1, which the C++ walks without end (none here, D863).
    let h = npc_ships::get(&cx, ship).unwrap().callback;
    assert_eq!(callbacks::flee_answer(&cx, h), None);

    // The dock guide: the destination is its dock target's position.
    let dock_target = solar_id(&cx, b"he01_03");
    let docker = guided(&mut cx, GuideDesc::from_meta(dock_meta), at);
    let (dg, guide) = guide_of(&cx, docker);
    assert_eq!(guides::guide_type(&cx, dg), 6);
    let GuideKind::Dock(goto, dock) = &guide.kind else {
        panic!("a dock guide");
    };
    assert_eq!(dock.dock_target, dock_target);
    let (position, _) = space_obj::location(&cx, dock_target).unwrap();
    assert_eq!(goto.destination, position);
    assert_eq!(goto.range, 600.0);
    assert!(!dock.docking);
    // Released with the ship: off the guided ships.
    let id = npc_ships::space_obj(&cx, docker);
    let _ = space_obj::destroy(&mut cx, id, 1);
    let _ = world::step_system(&mut cx, he01, 1.0 / 30.0);
    assert!(cx.logic.content.state.guides.get(dg).is_none());
    assert!(!cx.logic.content.state.guided_ships.contains(&Some(docker)));
}
