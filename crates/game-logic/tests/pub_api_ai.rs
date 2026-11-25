//! P3.3's `pub::AI` over the original data (`outdir/`, the rebranded
//! he01): every export's answers and result codes, a goto and a follow
//! submitted through `SubmitDirective` driving NPC fighters in he01 over a
//! few frames, the personality and the other state ops of `SubmitState`,
//! the player branch (`SETDIRECTIVE`, `SCANNOTIFY`) and the formation
//! broadcasts of `update_formation_state`. Expected values follow the C++
//! logic of `Server/server_pub.cpp:6946-7541`, `Common/common_pub.cpp`
//! (the ops' `validate`) and `Common/common_behaviormanager.cpp`.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::PubError;
use game_logic::api::ai::{
    self, BaseOp, CallbackArg, Op, OpRType, SetFlagParams, SetPersonalityParams,
    SetZoneBehaviorParams,
};
use game_logic::api::reputation;
use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::ai::behavior::{
    DirectiveOp, DockOp, FollowOp, GotoOp, LaunchOp, TractorBeamOp, TrailOp, ids,
};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

impl Shared {
    fn take(&self) -> Vec<(u32, ServerMessage)> {
        self.0
            .borrow_mut()
            .drain(..)
            .filter_map(|(c, m)| match m {
                ToClient::Server(s) => Some((c, s)),
                _ => None,
            })
            .collect()
    }
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn data(what: &str) -> Option<Arc<GameData>> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    Some(Arc::new(data))
}

fn setup(what: &str, sink: &Shared) -> Option<(World, Logic)> {
    let data = data(what)?;
    let world =
        World::new(data, WorldConfig::default()).with_assets(Assets::new(outdir().join("data")));
    let logic = Logic::new(Box::new(sink.clone()), 2, true);
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// A Content controller, which owns the ships.
fn content(cx: &mut Cx<'_>) -> u32 {
    neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0
}

/// he01 loaded with player 2 in it, so that the multiplayer frame steps
/// the physics.
fn he01_with_a_player(cx: &mut Cx<'_>) -> u32 {
    let controller = content(cx);
    world::load(cx, he01());
    let slot = cx.logic.players.get_mut(2).unwrap();
    slot.client_id = 2;
    slot.system_id = he01();
    world::add_player(cx, he01(), 2);
    controller
}

/// An NPC fighter made through `pub::SpaceObj::Create` at `at`.
fn fighter(cx: &mut Cx<'_>, controller: u32, at: Vector) -> (ObjHandle, u32) {
    let info = ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    let id = space_obj::create(cx, &info).expect("the ship is made");
    (cx.world.find(id).unwrap(), id)
}

/// Player 1's fighter in he01 (`ship_object_id` set).
fn player_ship(cx: &mut Cx<'_>, at: Vector) -> (ObjHandle, u32) {
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01();
    }
    let mut spawn = ShipSpawn::new(create_id(b"ge_fighter"), 0, at, Matrix::IDENTITY);
    spawn.owner_player = 1;
    let h = cx
        .world
        .spawn_ship(he01(), &spawn)
        .expect("ge_fighter spawns");
    if let Some(s) = cx.world.get_mut(h).and_then(|o| o.as_ship_mut()) {
        s.is_player = true;
    }
    let id = cx.world.get(h).unwrap().simple.id;
    cx.logic.players.get_mut(1).unwrap().ship_object_id = id;
    (h, id)
}

fn fighter_graph(cx: &Cx<'_>) -> i32 {
    cx.world
        .data()
        .state_graphs
        .find(b"FIGHTER", GraphType::Leader)
}

fn state_op(graph: i32) -> BaseOp {
    BaseOp::new(Op::SetPersonalityParams(Box::new(SetPersonalityParams {
        state_graph: graph,
        content_callback: CallbackArg::Null,
        directive_callback: CallbackArg::Null,
        ..SetPersonalityParams::default()
    })))
}

fn frames(cx: &mut Cx<'_>, n: usize) {
    for _ in 0..n {
        world::step_system(cx, he01(), 0.05);
        cx.world.update_global_time(0.05);
    }
}

fn position(cx: &Cx<'_>, h: ObjHandle) -> Vector {
    cx.world.get(h).unwrap().engine.position
}

fn distance(a: Vector, b: Vector) -> f64 {
    let (dx, dy, dz) = (
        f64::from(a.x) - f64::from(b.x),
        f64::from(a.y) - f64::from(b.y),
        f64::from(a.z) - f64::from(b.z),
    );
    (dx * dx + dy * dy + dz * dz).sqrt()
}

fn directive(cx: &Cx<'_>, h: ObjHandle) -> [i32; 3] {
    cx.world.behavior_manager(h).unwrap().state.directive
}

#[test]
fn every_op_validates_as_the_cpp() {
    let goto = |g: GotoOp| BaseOp::directive(DirectiveOp::Goto(g));
    // Goto: a destination with Field_1C, a negative range, cruise and no
    // cruise together, Field_64 below Field_60.
    assert!(ai::validate(&goto(GotoOp::default())));
    assert!(!ai::validate(&goto(GotoOp {
        field_1c: 1,
        destination: Vector::new(1.0, 0.0, 0.0),
        ..GotoOp::default()
    })));
    assert!(!ai::validate(&goto(GotoOp {
        range: -1.0,
        ..GotoOp::default()
    })));
    assert!(!ai::validate(&goto(GotoOp {
        cruise: 1,
        no_cruise: 1,
        ..GotoOp::default()
    })));
    assert!(!ai::validate(&goto(GotoOp {
        field_64: 100.0,
        ..GotoOp::default()
    })));
    // The ops with a target that must not be 0.
    let d = BaseOp::directive;
    assert!(!ai::validate(&d(DirectiveOp::Follow(FollowOp::default()))));
    assert!(!ai::validate(&d(DirectiveOp::Trail(TrailOp::default()))));
    assert!(!ai::validate(&d(DirectiveOp::TractorBeam(
        TractorBeamOp::default()
    ))));
    assert!(!ai::validate(&d(DirectiveOp::Evade(0))));
    assert!(ai::validate(&d(DirectiveOp::Evade(5))));
    assert!(!ai::validate(&BaseOp::new(Op::Strafe(0))));
    assert!(!ai::validate(&BaseOp::new(Op::Ram(0))));
    assert!(!ai::validate(&BaseOp::new(Op::WaitForShip(0))));
    assert!(ai::validate(&BaseOp::new(Op::Formation)));
    // Dock: an object; a granted port needs a port; the far threshold not
    // below the near one.
    let dock = |k: DockOp| d(DirectiveOp::Dock(k));
    assert!(!ai::validate(&dock(DockOp::default())));
    assert!(ai::validate(&dock(DockOp {
        dock_object: 9,
        ..DockOp::default()
    })));
    assert!(!ai::validate(&dock(DockOp {
        dock_object: 9,
        field_14: 1,
        ..DockOp::default()
    })));
    assert!(!ai::validate(&dock(DockOp {
        dock_object: 9,
        field_24: 100.0,
        ..DockOp::default()
    })));
    // Launch: an object, an index and a terminal.
    assert!(!ai::validate(&d(DirectiveOp::Launch(LaunchOp {
        launch_object: 9,
        index: 0,
        terminal: 0,
    }))));
    assert!(ai::validate(&d(DirectiveOp::Launch(LaunchOp {
        launch_object: 9,
        index: 0,
        terminal: 1,
    }))));
    assert!(!ai::validate(&d(DirectiveOp::Delay(0.0))));
    assert!(ai::validate(&d(DirectiveOp::Delay(1.0))));
    assert!(ai::validate(&d(DirectiveOp::Cancel)));
    assert!(ai::validate(&d(DirectiveOp::Idle)));
    // A known behaviour's name, case-insensitive.
    assert!(ai::validate(&BaseOp::new(Op::WaitForPlayerManeuver(
        b"goto".to_vec()
    ))));
    assert!(!ai::validate(&BaseOp::new(Op::WaitForPlayerManeuver(
        b"nothing".to_vec()
    ))));
    // The params: a state graph; a flag other than 2; a zone with a radius
    // and a point away from the origin (or an object), unless cleared.
    assert!(!ai::validate(&state_op(-1)));
    assert!(ai::validate(&state_op(0)));
    assert!(!ai::validate(&BaseOp::new(Op::SetFlagParams(
        SetFlagParams::default()
    ))));
    let zone = |z: SetZoneBehaviorParams| BaseOp::new(Op::SetZoneBehaviorParams(z));
    assert!(ai::validate(&zone(SetZoneBehaviorParams::default())));
    assert!(!ai::validate(&zone(SetZoneBehaviorParams {
        invert_mode: 0,
        ..SetZoneBehaviorParams::default()
    })));
    assert!(!ai::validate(&zone(SetZoneBehaviorParams {
        invert_mode: 0,
        radius: 10.0,
        ..SetZoneBehaviorParams::default()
    })));
    assert!(ai::validate(&zone(SetZoneBehaviorParams {
        invert_mode: 0,
        radius: 10.0,
        target_id: 3,
        ..SetZoneBehaviorParams::default()
    })));
    assert!(ai::validate(&zone(SetZoneBehaviorParams {
        invert_mode: 1,
        radius: 10.0,
        target_point: Vector::new(0.0, 0.0, 1.0),
        ..SetZoneBehaviorParams::default()
    })));
}

#[test]
fn the_answers_without_an_object() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::AI without an object", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    world::load(&mut cx, he01());
    let missing = 0x1234_5678;
    let idle = BaseOp::directive(DirectiveOp::Idle);
    // A zero id (its low 30 bits) is 4 for both; the op is validated before
    // the object is looked up.
    assert_eq!(
        ai::submit_state(&mut cx, 0xc000_0000, &state_op(0)),
        OpRType(4)
    );
    assert_eq!(ai::submit_state(&mut cx, missing, &state_op(0)), OpRType(4));
    assert_eq!(
        ai::submit_directive(&mut cx, 0x4000_0000, &idle),
        OpRType(4)
    );
    assert_eq!(
        ai::submit_directive(&mut cx, missing, &BaseOp::new(Op::Ram(0))),
        OpRType(5)
    );
    assert_eq!(ai::submit_directive(&mut cx, missing, &idle), OpRType(5));
    assert_eq!(ai::set_directive_priority(&mut cx, missing, 2), OpRType(5));
    assert_eq!(ai::behavior_id(&mut cx, missing), -1);
    assert_eq!(ai::state_graph_id(&mut cx, missing), -1);
    assert_eq!(ai::personality(&mut cx, missing), None);
    assert!(!ai::enable_maneuver(&mut cx, missing, 2, true));
    assert!(!ai::enable_all_maneuvers(&mut cx, missing));
    assert!(!ai::lock_maneuvers(&mut cx, missing, true));
    assert_eq!(ai::scan_response(&mut cx, 1, missing, 2), 0);
    assert_eq!(ai::submit_forced_target(&mut cx, missing, 1), 0);
    assert_eq!(ai::remove_forced_target(&mut cx, missing, 1), 0);
    assert!(!ai::set_player_enemy_clamp(&mut cx, missing, 0, 1));
    assert_eq!(
        ai::update_formation_state(&mut cx, missing, 0, Vector::ZERO),
        Err(PubError::NOT_FOUND)
    );
    ai::refresh_state_graph(&mut cx, missing);
    assert!(sink.take().is_empty());
}

#[test]
fn an_npc_in_he01_takes_its_state_and_its_directives() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::AI on an NPC", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let start = Vector::new(15000.0, 3000.0, -25000.0);
    let (ship, ship_id) = fighter(&mut cx, controller, start);
    let graph = fighter_graph(&cx);
    assert!(graph >= 0);

    // `SetPersonalityParams`: the state graph, the callbacks, no
    // personality (0).
    let before = ai::personality(&mut cx, ship_id).expect("a manager");
    let op = BaseOp::new(Op::SetPersonalityParams(Box::new(SetPersonalityParams {
        state_graph: graph,
        content_callback: CallbackArg::Set(2),
        directive_callback: CallbackArg::Set(77),
        ..SetPersonalityParams::default()
    })));
    assert_eq!(ai::submit_state(&mut cx, ship_id, &op), OpRType::SUCCESS);
    assert_eq!(ai::state_graph_id(&mut cx, ship_id), graph);
    let m = cx.world.behavior_manager(ship).unwrap();
    assert!(m.state.content_callback);
    assert_eq!(m.state.content_handle, 2);
    // Content has no object of handle 2: the base's creation type, 3.
    assert_eq!(m.state.creation_type, 3);
    assert_eq!(m.state.directive_callbacks, [77, 0, 0, 0, 0]);
    assert_eq!(ai::personality(&mut cx, ship_id), Some(before));
    // The same callback again keeps its slot; -1 keeps both; null clears
    // the content callback and leaves the slots.
    let mut again = SetPersonalityParams {
        state_graph: graph,
        directive_callback: CallbackArg::Set(77),
        ..SetPersonalityParams::default()
    };
    assert_eq!(
        ai::submit_state(
            &mut cx,
            ship_id,
            &BaseOp::new(Op::SetPersonalityParams(Box::new(again.clone())))
        ),
        OpRType::SUCCESS
    );
    let m = cx.world.behavior_manager(ship).unwrap();
    assert!(m.state.content_callback);
    assert_eq!(m.state.directive_callbacks, [77, 0, 0, 0, 0]);
    again.directive_callback = CallbackArg::Null;
    again.content_callback = CallbackArg::Null;
    ai::submit_state(
        &mut cx,
        ship_id,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(again))),
    );
    let m = cx.world.behavior_manager(ship).unwrap();
    assert!(!m.state.content_callback);
    assert_eq!(m.state.directive_callbacks, [77, 0, 0, 0, 0]);

    // The personality changes: `get_personality` answers the new one.
    let mut personality = before;
    personality.job.loot_flee_threshold = 0;
    let op = BaseOp::new(Op::SetPersonalityParams(Box::new(SetPersonalityParams {
        state_graph: graph,
        set_personality: true,
        personality,
        ..SetPersonalityParams::default()
    })));
    assert_eq!(ai::submit_state(&mut cx, ship_id, &op), OpRType::SUCCESS);
    assert_eq!(ai::personality(&mut cx, ship_id), Some(personality));
    assert_ne!(personality, before);

    // A user zone: the box's centre and half extents (4); a sphere; a live
    // object's sphere; invert mode 2 clears it (0).
    let zone = |z: SetZoneBehaviorParams| BaseOp::new(Op::SetZoneBehaviorParams(z));
    let op = zone(SetZoneBehaviorParams {
        mode: 1,
        invert_mode: 1,
        box_min: Vector::new(-10.0, 0.5, 3.0),
        box_max: Vector::new(30.0, 1.0, -5.0),
        ..SetZoneBehaviorParams::default()
    });
    assert_eq!(ai::submit_state(&mut cx, ship_id, &op), OpRType(4));
    let z = cx
        .world
        .behavior_manager(ship)
        .unwrap()
        .state
        .pathfind
        .user_zone
        .clone()
        .expect("a zone");
    assert_eq!((z.mode, z.invert), (1, 1));
    assert_eq!(z.box_center, Vector::new(10.0, 0.75, -1.0));
    assert_eq!(z.box_half_extent, Vector::new(20.0, 0.25, 4.0));
    assert_eq!(z.box_rotation, Matrix::IDENTITY);
    let op = zone(SetZoneBehaviorParams {
        mode: 2,
        invert_mode: 0,
        target_id: 99,
        radius: 400.0,
        target_point: Vector::new(1.0, 2.0, 3.0),
        ..SetZoneBehaviorParams::default()
    });
    assert_eq!(ai::submit_state(&mut cx, ship_id, &op), OpRType(4));
    let z = cx
        .world
        .behavior_manager(ship)
        .unwrap()
        .state
        .pathfind
        .user_zone
        .clone()
        .unwrap();
    assert_eq!(
        (z.mode, z.invert, z.target_key, z.radius),
        (2, 0, 99, 400.0)
    );
    assert_eq!(z.target_point, Vector::ZERO, "mode 2 takes no point");
    assert_eq!(
        ai::submit_state(&mut cx, ship_id, &zone(SetZoneBehaviorParams::default())),
        OpRType::SUCCESS
    );
    assert!(
        cx.world
            .behavior_manager(ship)
            .unwrap()
            .state
            .pathfind
            .user_zone
            .is_none()
    );

    // Flags: 0 the pathfinder's Flag245, 1 the fire control; 4 either way.
    let flag =
        |flag_id: i32, value: u8| BaseOp::new(Op::SetFlagParams(SetFlagParams { flag_id, value }));
    assert_eq!(ai::submit_state(&mut cx, ship_id, &flag(0, 0)), OpRType(4));
    assert_eq!(ai::submit_state(&mut cx, ship_id, &flag(1, 0)), OpRType(4));
    assert_eq!(ai::submit_state(&mut cx, ship_id, &flag(5, 1)), OpRType(4));
    let m = cx.world.behavior_manager(ship).unwrap();
    assert_eq!(m.state.pathfind.flag_245, 0);
    assert!(!m.state.sys.guns.enabled);
    ai::submit_state(&mut cx, ship_id, &flag(1, 1));
    assert!(
        cx.world
            .behavior_manager(ship)
            .unwrap()
            .state
            .sys
            .guns
            .enabled
    );
    // A directive is not a state op.
    assert_eq!(
        ai::submit_state(&mut cx, ship_id, &BaseOp::directive(DirectiveOp::Idle)),
        OpRType(4)
    );

    // A goto through `SubmitDirective`: accepted (1), the goto behaviour
    // runs and the ship closes on the point over a few frames.
    let target = Vector::new(15000.0, 3000.0, -27000.0);
    let goto = BaseOp {
        priority: 2,
        op: Op::Directive(DirectiveOp::Goto(GotoOp {
            kind: 1,
            destination: target,
            range: 100.0,
            no_cruise: 1,
            ..GotoOp::default()
        })),
    };
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &goto),
        OpRType::ACCEPTED
    );
    assert_eq!(directive(&cx, ship), [1, 2, ids::GOTO]);
    assert_eq!(ai::behavior_id(&mut cx, ship_id), ids::GOTO);
    let far = distance(position(&cx, ship), target);
    frames(&mut cx, 60);
    let near = distance(position(&cx, ship), target);
    assert_eq!(ai::behavior_id(&mut cx, ship_id), ids::GOTO);
    assert!(
        near < far - 50.0,
        "the ship flew toward the point: {far} to {near}"
    );

    // A current directive blocks another (2); its priority changes (1).
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &BaseOp::directive(DirectiveOp::Idle)),
        OpRType::DIRECTIVE_BUSY
    );
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &BaseOp::new(Op::Strafe(5))),
        OpRType::DIRECTIVE_BUSY
    );
    assert_eq!(
        ai::set_directive_priority(&mut cx, ship_id, 4),
        OpRType::ACCEPTED
    );
    assert_eq!(directive(&cx, ship), [1, 4, ids::GOTO]);
    // An invalid op is refused first (5).
    assert_eq!(
        ai::submit_directive(
            &mut cx,
            ship_id,
            &BaseOp::directive(DirectiveOp::Trail(TrailOp::default()))
        ),
        OpRType::INVALID
    );
    // A cancel always goes through (1) and returns to the fallback; then no
    // directive has a priority (3).
    let cancel = BaseOp::directive(DirectiveOp::Cancel);
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &cancel),
        OpRType::ACCEPTED
    );
    assert_eq!(directive(&cx, ship), [0, 0, 0]);
    assert_eq!(
        ai::set_directive_priority(&mut cx, ship_id, 4),
        OpRType::NO_DIRECTIVE
    );
    // Ops without a behaviour here: 5.
    for op in [Op::Strafe(5), Op::Ram(5), Op::Formation, Op::WaitForShip(5)] {
        assert_eq!(
            ai::submit_directive(&mut cx, ship_id, &BaseOp::new(op)),
            OpRType::INVALID
        );
    }
    // A wait for a player's manoeuvre is directive kind 2 with the
    // behaviour's id; it blocks the next op.
    let wait = BaseOp {
        priority: 1,
        op: Op::WaitForPlayerManeuver(b"Trail".to_vec()),
    };
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &wait),
        OpRType::ACCEPTED
    );
    assert_eq!(directive(&cx, ship), [2, 1, ids::TRAIL]);
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &goto),
        OpRType::DIRECTIVE_BUSY
    );
    // `refresh_state_graph` sets the graph again by force; the database
    // answers the same graph, so the directive stays.
    ai::refresh_state_graph(&mut cx, ship_id);
    assert_eq!(directive(&cx, ship), [2, 1, ids::TRAIL]);
    assert_eq!(ai::state_graph_id(&mut cx, ship_id), graph);
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &cancel),
        OpRType::ACCEPTED
    );

    // A follow: a second fighter follows the first and closes in.
    let (follower, follower_id) =
        fighter(&mut cx, controller, Vector::new(15000.0, 3000.0, -23500.0));
    assert_eq!(
        ai::submit_state(&mut cx, follower_id, &state_op(graph)),
        OpRType::SUCCESS
    );
    let follow = BaseOp::directive(DirectiveOp::Follow(FollowOp {
        target: ship_id,
        ..FollowOp::default()
    }));
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &follow),
        OpRType::ACCEPTED
    );
    assert_eq!(ai::behavior_id(&mut cx, follower_id), ids::FOLLOW);
    let far = distance(position(&cx, follower), position(&cx, ship));
    frames(&mut cx, 80);
    let near = distance(position(&cx, follower), position(&cx, ship));
    assert_eq!(ai::behavior_id(&mut cx, follower_id), ids::FOLLOW);
    assert!(near < far - 50.0, "the follower closed in: {far} to {near}");

    // A trail carries its object to the behaviour: an object that is not
    // there is refused by the behaviour (5, the directive stays set), one
    // that is accepted (1).
    let trail = |target: u32| {
        BaseOp::directive(DirectiveOp::Trail(TrailOp {
            target,
            distance: 300.0,
            ..TrailOp::default()
        }))
    };
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &cancel),
        OpRType::ACCEPTED
    );
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &trail(0x7000_0001)),
        OpRType::INVALID
    );
    assert_eq!(directive(&cx, follower), [1, 0, ids::TRAIL]);
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &cancel),
        OpRType::ACCEPTED
    );
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &trail(ship_id)),
        OpRType::ACCEPTED
    );
    assert_eq!(ai::behavior_id(&mut cx, follower_id), ids::TRAIL);

    // The manoeuvres: the mask's bit `id + 1`, all enabled, the lock.
    assert!(ai::enable_maneuver(&mut cx, ship_id, 3, false));
    assert_eq!(
        cx.world.behavior_manager(ship).unwrap().state.maneuver_mask,
        1 << 4
    );
    assert!(
        ai::enable_maneuver(&mut cx, ship_id, -1, false),
        "true though refused"
    );
    assert!(ai::enable_all_maneuvers(&mut cx, ship_id));
    assert_eq!(
        cx.world.behavior_manager(ship).unwrap().state.maneuver_mask,
        0
    );
    assert!(ai::lock_maneuvers(&mut cx, ship_id, true));
    assert!(cx.world.behavior_manager(ship).unwrap().state.lock);
    assert!(ai::lock_maneuvers(&mut cx, ship_id, false));

    // Forced targets: eight at most, each once.
    assert_eq!(ai::submit_forced_target(&mut cx, ship_id, follower_id), 1);
    assert_eq!(ai::submit_forced_target(&mut cx, ship_id, follower_id), 0);
    for t in 1..=7 {
        assert_eq!(
            ai::submit_forced_target(&mut cx, ship_id, 0x7000_0000 + t),
            1
        );
    }
    assert_eq!(
        ai::submit_forced_target(&mut cx, ship_id, 0x7000_0008),
        0,
        "full"
    );
    assert_eq!(ai::remove_forced_target(&mut cx, ship_id, follower_id), 1);
    assert_eq!(ai::remove_forced_target(&mut cx, ship_id, follower_id), 0);
    assert_eq!(
        cx.world
            .behavior_manager(ship)
            .unwrap()
            .state
            .sys
            .targeting
            .forced_targets,
        (1..=7).map(|t| 0x7000_0000 + t).collect::<Vec<u32>>()
    );
    assert!(ai::set_player_enemy_clamp(&mut cx, ship_id, -1, 3));
    assert_eq!(
        cx.world
            .behavior_manager(ship)
            .unwrap()
            .state
            .sys
            .targeting
            .enemy_clamp,
        (-1, 3)
    );

    // The scan response: the hostile power ratio around the ship is 0
    // (both fighters were made without a reputation group, so their
    // toughness is 0 and the total is not above 0.01), below any
    // loot-flee threshold: 1, even with the follower hostile both ways and
    // the lowest threshold (kind 0, 0.15).
    // No client hears of it.
    assert_eq!(ai::scan_response(&mut cx, follower_id, ship_id, 0), 1);
    cx.world.set_attitude(ship, follower, -1.0);
    cx.world.set_attitude(follower, ship, -1.0);
    assert_eq!(personality.job.loot_flee_threshold, 0);
    assert_eq!(ai::scan_response(&mut cx, follower_id, ship_id, 0), 1);
    assert!(!sink.take().iter().any(|(_, m)| matches!(
        m,
        ServerMessage::ScanNotify(_) | ServerMessage::SetDirective(_)
    )));
}

#[test]
fn a_players_ship_hears_its_directive_and_its_scan() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::AI on a player's ship", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (ship, ship_id) = player_ship(&mut cx, Vector::new(15000.0, 3000.0, -25000.0));
    let (_, npc_id) = fighter(&mut cx, controller, Vector::new(15200.0, 3000.0, -25000.0));
    sink.take();
    let manager_before = cx.world.behavior_manager(ship).map(|m| m.state.directive);

    // A goto goes to the owner as SETDIRECTIVE kind 4 with the op's fields.
    let goto = BaseOp::directive(DirectiveOp::Goto(GotoOp {
        kind: 1,
        destination: Vector::new(1.0, 2.0, 3.0),
        range: 250.0,
        cruise: 1,
        field_5c: npc_id,
        words_20: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
        field_6c: 1,
        ..GotoOp::default()
    }));
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &goto),
        OpRType::ACCEPTED
    );
    let sent = sink.take();
    let [(1, ServerMessage::SetDirective(d))] = &sent[..] else {
        panic!("{sent:?}");
    };
    let d = &d.directive;
    assert_eq!(d.field_00, 4);
    assert_eq!(d.field_40, 1);
    assert_eq!(d.field_44, [1.0, 2.0, 3.0]);
    assert_eq!(d.blocks, [[1, 2, 3], [4, 5, 6], [7, 8, 9], [10, 11, 12]]);
    assert_eq!((d.field_84, d.field_88), (250.0, -1.0));
    assert_eq!(d.tail, [0, 1, 1, 0, 1]);
    assert_eq!((d.field_1c, d.field_20, d.field_24), (npc_id, 200.0, 500.0));
    assert_eq!((d.field_18, d.field_28), (-1, 0), "the constructor's seeds");

    // A follow (3), a dock (2), a delay (1), an idle (5) and a cancel (0).
    let follow = BaseOp::directive(DirectiveOp::Follow(FollowOp {
        target: npc_id,
        distance: 80.0,
        offset: Vector::new(5.0, 6.0, 7.0),
        range: 300.0,
    }));
    ai::submit_directive(&mut cx, ship_id, &follow);
    let dock = BaseOp::directive(DirectiveOp::Dock(DockOp {
        dock_object: npc_id,
        field_10: 3,
        field_14: 1,
        port: 2,
        field_1c: 4,
        ..DockOp::default()
    }));
    ai::submit_directive(&mut cx, ship_id, &dock);
    ai::submit_directive(
        &mut cx,
        ship_id,
        &BaseOp::directive(DirectiveOp::Delay(2.5)),
    );
    ai::submit_directive(&mut cx, ship_id, &BaseOp::directive(DirectiveOp::Idle));
    ai::submit_directive(&mut cx, ship_id, &BaseOp::directive(DirectiveOp::Cancel));
    let sent: Vec<_> = sink
        .take()
        .into_iter()
        .map(|(c, m)| match m {
            ServerMessage::SetDirective(d) => (c, d.directive),
            other => panic!("{other:?}"),
        })
        .collect();
    assert_eq!(sent.len(), 5);
    assert!(sent.iter().all(|(c, _)| *c == 1));
    let f = &sent[0].1;
    assert_eq!(
        (
            f.field_00, f.field_28, f.field_2c, f.field_30, f.field_3c, f.field_40
        ),
        (3, npc_id, 80.0, [5.0, 6.0, 7.0], 300.0, 3)
    );
    let k = &sent[1].1;
    assert_eq!(
        (
            k.field_00, k.field_10, k.field_14, k.field_08, k.field_18, k.field_1c, k.field_20,
            k.field_24
        ),
        (2, npc_id, 3, 1, 2, 4, 200.0, 500.0)
    );
    assert_eq!((sent[2].1.field_00, sent[2].1.field_0c), (1, 2.5));
    assert_eq!(sent[3].1.field_00, 5);
    assert_eq!(sent[4].1.field_00, 0);

    // Any other op is refused (5), with nothing sent; the manager is not
    // touched.
    let trail = BaseOp::directive(DirectiveOp::Trail(TrailOp {
        target: npc_id,
        ..TrailOp::default()
    }));
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &trail),
        OpRType::INVALID
    );
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &BaseOp::new(Op::Formation)),
        OpRType::INVALID
    );
    assert!(sink.take().is_empty());
    assert_eq!(
        cx.world.behavior_manager(ship).map(|m| m.state.directive),
        manager_before
    );

    // The scan: the owner hears the scanner and the good; 3.
    assert_eq!(ai::scan_response(&mut cx, npc_id, ship_id, 0x77), 3);
    let sent = sink.take();
    assert!(
        matches!(&sent[..], [(1, ServerMessage::ScanNotify(s))] if s.field_00 == npc_id && s.field_04 == 0x77),
        "{sent:?}"
    );

    // Once the slot flies another ship, the ship is an AI's again.
    cx.logic.players.get_mut(1).unwrap().ship_object_id = npc_id;
    assert_eq!(
        ai::submit_directive(&mut cx, ship_id, &BaseOp::directive(DirectiveOp::Idle)),
        OpRType::ACCEPTED
    );
    assert!(sink.take().is_empty());
    assert_eq!(directive(&cx, ship), [1, 0, ids::IDLE]);
}

#[test]
fn a_formation_change_tells_the_players_in_it() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::AI formations", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (leader, leader_id) = fighter(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let (wing, wing_id) = player_ship(&mut cx, Vector::new(14955.0, 3000.0, -24986.0));
    sink.take();
    let formation = |sent: Vec<(u32, ServerMessage)>| -> Vec<(u32, u16, Vec<u32>)> {
        sent.into_iter()
            .filter_map(|(c, m)| match m {
                ServerMessage::MiscObjUpdate(u) => Some((c, u.flags, u.ids)),
                _ => None,
            })
            .collect()
    };

    // The leader leads itself: no follower, an NPC: nobody hears.
    assert_eq!(
        ai::update_formation_state(&mut cx, leader_id, leader_id, Vector::ZERO),
        Ok(())
    );
    assert!(formation(sink.take()).is_empty());
    // The player's ship joins: it hears the members, the leader first.
    let offset = Vector::new(-45.0, 0.0, 14.0);
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, leader_id, offset),
        Ok(())
    );
    assert_eq!(
        formation(sink.take()),
        [(1, 0x100, vec![leader_id, wing_id])]
    );
    let followers = &cx
        .world
        .get(leader)
        .unwrap()
        .as_ship()
        .unwrap()
        .formation_followers;
    assert_eq!(followers, &[Some(wing)]);
    let w = cx.world.get(wing).unwrap().as_ship().unwrap();
    assert_eq!(
        (w.formation_leader, w.formation_offset),
        (Some(leader), offset)
    );
    // The same leader again changes nothing on a multiplayer server.
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, leader_id, Vector::ZERO),
        Ok(())
    );
    assert!(formation(sink.take()).is_empty());
    assert_eq!(
        cx.world
            .get(wing)
            .unwrap()
            .as_ship()
            .unwrap()
            .formation_offset,
        offset
    );
    // It leaves: the old formation's members without it (the leader, as it
    // had a follower), told before it went.
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, 0, Vector::ZERO),
        Ok(())
    );
    assert_eq!(formation(sink.take()), [(1, 0x100, vec![leader_id])]);
    assert!(
        cx.world
            .get(leader)
            .unwrap()
            .as_ship()
            .unwrap()
            .formation_followers
            .iter()
            .all(Option::is_none)
    );
    assert_eq!(
        cx.world
            .get(wing)
            .unwrap()
            .as_ship()
            .unwrap()
            .formation_leader,
        None
    );
    // The player's ship leads itself: it hears a formation of one; it
    // then hears an empty list when it leaves it (no flag, no ids).
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, wing_id, Vector::ZERO),
        Ok(())
    );
    assert_eq!(formation(sink.take()), [(1, 0x100, vec![wing_id])]);
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, 0, Vector::ZERO),
        Ok(())
    );
    assert_eq!(formation(sink.take()), [(1, 0, vec![])]);
}

/// An NPC fighter of `rank` in its own reputation group
/// (`pub::Reputation::Alloc`, `ShipInfo::rank`, which `CShip::init`
/// gives the group with `Vibe::SetRank`).
fn ranked_fighter(cx: &mut Cx<'_>, controller: u32, at: Vector, rank: u32) -> (ObjHandle, u32) {
    let name = FmtStr::new(0);
    let vibe = reputation::alloc(cx, &name, &name).expect("a vibe");
    let mut info = ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    info.vibe = vibe;
    info.rank = rank;
    let id = space_obj::create(cx, &info).expect("the ship is made");
    (cx.world.find(id).unwrap(), id)
}

#[test]
fn the_scan_weighs_the_toughness_of_the_ships_around() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the scan's toughness", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let at = |x: f32| Vector::new(15000.0 + x, 3000.0, -25000.0);
    let (ship, ship_id) = ranked_fighter(&mut cx, controller, at(0.0), 3);
    let (hostile, _) = ranked_fighter(&mut cx, controller, at(300.0), 2);
    let (scanner, scanner_id) = ranked_fighter(&mut cx, controller, at(-300.0), 7);
    let graph = fighter_graph(&cx);
    assert_eq!(
        ai::submit_state(&mut cx, ship_id, &state_op(graph)),
        OpRType::SUCCESS
    );
    // `get_toughness`: each ship's group rank.
    assert_eq!(cx.world.ai_view(ship).unwrap().toughness, 3);
    assert_eq!(cx.world.ai_view(hostile).unwrap().toughness, 2);
    assert_eq!(cx.world.ai_view(scanner).unwrap().toughness, 7);
    let threshold = |cx: &mut Cx<'_>, kind: i32| {
        let mut personality = ai::personality(cx, ship_id).expect("a manager");
        personality.job.loot_flee_threshold = kind;
        let op = BaseOp::new(Op::SetPersonalityParams(Box::new(SetPersonalityParams {
            state_graph: graph,
            set_personality: true,
            personality,
            ..SetPersonalityParams::default()
        })));
        assert_eq!(ai::submit_state(cx, ship_id, &op), OpRType::SUCCESS);
    };
    let scan = |cx: &mut Cx<'_>| ai::scan_response(cx, scanner_id, ship_id, 0);

    // `GetHostilePowerRatio`: nobody hostile or friendly, so the hostile
    // sum is 0 and the ratio 0 / 3: below every threshold, 1.
    threshold(&mut cx, 0);
    assert_eq!(scan(&mut cx), 1);
    // The ship hostile to one of rank 2: 2 / (3 + 2) = 0.4, below 0.65
    // (kind 2) but not below 0.35 (kind 1).
    cx.world.set_attitude(ship, hostile, -1.0);
    cx.world.set_attitude(hostile, ship, 0.0);
    threshold(&mut cx, 2);
    assert_eq!(scan(&mut cx), 1);
    threshold(&mut cx, 1);
    assert_eq!(scan(&mut cx), 0);
    // The scanner (rank 7) hostile to the ship only one way (its reverse
    // attitude): 9 / 12 = 0.75, not below 0.65, below 0.85 (kind 3).
    cx.world.set_attitude(scanner, ship, -0.6);
    cx.world.set_attitude(ship, scanner, 0.0);
    threshold(&mut cx, 2);
    assert_eq!(scan(&mut cx), 0);
    threshold(&mut cx, 3);
    assert_eq!(scan(&mut cx), 1);
    // The scanner friendly instead (reverse 0.6 and the ship's -0.5 not
    // hostile): it adds to the own side, 2 / (3 + 7 + 2) = 0.1667, below
    // 0.35 (kind 1) and not below 0.15 (kind 0).
    cx.world.set_attitude(scanner, ship, 0.6);
    cx.world.set_attitude(ship, scanner, -0.5);
    threshold(&mut cx, 1);
    assert_eq!(scan(&mut cx), 1);
    threshold(&mut cx, 0);
    assert_eq!(scan(&mut cx), 0);
    // A rank set at run time (`pub::Reputation::SetRank`) is read: the
    // hostile of rank 20 gives 20 / 30 = 0.6667, not below 0.65.
    let vibe = cx
        .world
        .get(hostile)
        .and_then(|o| o.as_equipped())
        .unwrap()
        .vibe;
    reputation::set_rank(&mut cx, vibe, 20.0).unwrap();
    assert_eq!(cx.world.ai_view(hostile).unwrap().toughness, 20);
    threshold(&mut cx, 2);
    assert_eq!(scan(&mut cx), 0);
    // Every rank 0: the total is not above 0.01, the ratio 0, so 1 even
    // with the lowest threshold.
    for h in [ship, hostile, scanner] {
        let vibe = cx.world.get(h).and_then(|o| o.as_equipped()).unwrap().vibe;
        reputation::set_rank(&mut cx, vibe, 0.0).unwrap();
    }
    threshold(&mut cx, 0);
    assert_eq!(scan(&mut cx), 1);
}
