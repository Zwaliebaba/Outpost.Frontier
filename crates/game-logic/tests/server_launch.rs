//! An NPC's launch over the original data (`outdir/`, the rebranded he01):
//! `pub::SpaceObj::Launch` → `StarSystem::launch` → `CShip::launch` and
//! its `LaunchAction` (`Common/common_cship.cpp:2090`,
//! `Common/common_launchaction.cpp`), out of the docking ring
//! `Dock_Ring_he01_01` (the planet he01_01's, base he01_01_Base) and the
//! station he01_14, with the launch invincibility that `Ship::slot_70`
//! (`LaunchComplete`) lifts at the end, and the bare world's launch without
//! a server. Expected values follow the C++ logic.

use std::cell::RefCell;
use std::path::Path;
use std::sync::Arc;

use game_logic::api::ai::{self, BaseOp, CallbackArg, Op, OpRType, SetPersonalityParams};
use game_logic::api::space_obj::{self, InvincibleState, ShipInfo};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::ai::behavior::ids;
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{
    Assets, ControllerRef, LAUNCH_ACTION_CALLBACK, ShipSpawn, WorldConfig, WorldEvent,
};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

struct Sink;

impl ClientSink for Sink {
    fn send(&mut self, _client: u32, _message: ToClient) {}
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(what: &str) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(Sink), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        if let ControllerMessage::LaunchComplete { id, ship } = message {
            HEARD.with(|h| h.borrow_mut().push(format!("LaunchComplete {id} {ship}")));
        }
    });
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// he01 loaded with player 2 in it, so that the multiplayer frame steps
/// the physics; a Content controller, which owns the NPCs.
fn he01_with_a_player(cx: &mut Cx<'_>) -> u32 {
    let controller = neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0;
    world::load(cx, he01());
    let slot = cx.logic.players.get_mut(2).unwrap();
    slot.client_id = 2;
    slot.system_id = he01();
    world::add_player(cx, he01(), 2);
    controller
}

fn solar(cx: &Cx<'_>, nickname: &[u8]) -> ObjHandle {
    cx.world.find(create_id(nickname)).expect("the solar")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

/// An NPC fighter made through `pub::SpaceObj::Create` at `at`.
fn fighter(cx: &mut Cx<'_>, controller: u32, at: Vector) -> ObjHandle {
    let info = ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    let id = space_obj::create(cx, &info).expect("the ship is made");
    cx.world.find(id).unwrap()
}

fn position(cx: &Cx<'_>, h: ObjHandle) -> Vector {
    cx.world.get(h).unwrap().engine.position
}

fn collisions(world: &World, h: ObjHandle) -> bool {
    let (system, body) = world.body(h).expect("a body");
    world.physics(system).unwrap().collision_state(body)
}

fn has_controller(world: &World, h: ObjHandle, which: ControllerRef) -> bool {
    world
        .get(h)
        .unwrap()
        .physical
        .controllers
        .iter()
        .any(|(w, _, _)| *w == which)
}

fn exclusion(world: &World, h: ObjHandle) -> u32 {
    world
        .get(h)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .control_exclusion
}

fn throttle(world: &World, h: ObjHandle) -> f32 {
    world.get(h).unwrap().as_ship().unwrap().flight.throttle
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `LaunchAction::Start`'s path end for a ship at `at` with its `hpmount`
/// at `mount`: the launch point less 150 times the launch orientation's
/// third column, plus `at - mount`, each float store rounded.
fn path_end(point: Vector, orient: &Matrix, at: Vector, mount: Vector) -> Vector {
    let m = &orient.m;
    let back = |p: f32, c: f32| r(e(p) + e(-c) * 150.0);
    let p = Vector::new(
        back(point.x, m[0][2]),
        back(point.y, m[1][2]),
        back(point.z, m[2][2]),
    );
    Vector::new(
        r(e(p.x) + (e(at.x) - e(mount.x))),
        r(e(p.y) + (e(at.y) - e(mount.y))),
        r(e(p.z) + (e(at.z) - e(mount.z))),
    )
}

/// `LaunchAction::do_simulation_controller`'s beam point at coast time
/// `t`: `X = 150 f² + 80 (2.5 - t) + 40` (`f = (2.5 - t) / 2.5`) while
/// `t < 2.5`, else `(3 - t) / 0.5 · 40`, along the direction from the end.
fn path_point(end: Vector, dir: Vector, t: f32) -> Vector {
    let x = if t < 2.5 {
        let rem = 2.5 - e(t);
        let f = rem / 2.5;
        (150.0 * f * f + 80.0 * rem) + 40.0
    } else {
        ((3.0 - e(t)) / 0.5) * 40.0
    };
    let sy = r(x * e(dir.y));
    let sz = r(x * e(dir.z));
    Vector::new(
        r(x * e(dir.x) + e(end.x)),
        r(e(sy) + e(end.y)),
        r(e(sz) + e(end.z)),
    )
}

/// The frame of the server thread (1/30 s): one PSI of the frame's
/// length on a multiplayer server (`IVP_Environment_SimulateDTime`, fact
/// F16).
const FRAME: f32 = 1.0 / 30.0;

/// The frame whose PSI takes the coast time to 3 s: the float sum of 90
/// PSIs of 1/30 s stays below 3 (2.9999...), the 91st gives 3.0333, so
/// the ship's update in frame 92 completes the launch.
const DONE_AFTER: usize = 91;

/// The coast time after `n` frames: the float sum the controller stores.
fn coast_after(n: usize) -> f32 {
    (0..n).fold(0.0f32, |t, _| r(e(t) + e(FRAME)))
}

#[test]
fn an_npc_launches_from_a_docking_ring() {
    let Some((mut world, mut logic)) = setup("a ring's launch") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let ring = solar(&cx, b"Dock_Ring_he01_01");
    let ring_id = id(&cx, ring);
    assert_eq!(cx.world.get(ring).unwrap().simple.type_bits, 0x20);
    // The ship waits beside the ring; Content launches it from point 0,
    // which a ring turns into point 1.
    let start = Vector::new(-33000.0, 100.0, -28500.0);
    let npc = fighter(&mut cx, controller, start);
    let npc_id = id(&cx, npc);
    let (_, point, orient) = cx.world.launch_pos(ring, 1);
    let (mount, _) = cx.world.hardpoint_world(npc, b"hpmount").expect("hpmount");
    let end = path_end(point, &orient, start, mount);
    let dir = Vector::new(orient.m[0][2], orient.m[1][2], orient.m[2][2]);
    let _ = heard();

    space_obj::launch(&mut cx, npc_id, ring_id, 0).unwrap();
    let state = cx
        .world
        .launch_state(npc)
        .expect("the launch action")
        .clone();
    assert!(state.is_physical() && !state.directive);
    assert_eq!(
        (state.base, state.flags, state.launch_type),
        (ring_id, 0x20, 1)
    );
    assert_eq!((state.position, state.direction), (end, dir));
    assert_eq!(state.orientation, orient);
    assert_eq!(state.coast_timer, 40.0);
    assert!(!state.no_docking_camera, "dock_ring has a docking camera");
    assert_eq!(state.coast_elapsed, 0.0);
    // `Start`: collisions off, exclusion 7, throttle 0, the controller on;
    // then `apply_launch_invincibility`.
    assert!(!collisions(cx.world, npc));
    assert_eq!(exclusion(cx.world, npc), 7);
    assert_eq!(throttle(cx.world, npc), 0.0);
    assert!(has_controller(cx.world, npc, ControllerRef::Launch));
    assert_eq!(
        space_obj::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: true,
            protect: true,
            max_hp_loss: 0.0
        })
    );

    // Frames of 1/30 s, one PSI each. Each PSI beams the ship onto the
    // path for its coast time; the update after the PSI that reaches 3 s
    // completes the launch.
    let done_after = (1..).find(|&k| coast_after(k) >= 3.0).unwrap();
    let mut frame = 0;
    let mut previous = position(&cx, npc);
    loop {
        frame += 1;
        world::physics_update(&mut cx, FRAME);
        cx.world.update_global_time(FRAME);
        let lines = heard();
        if frame <= done_after {
            assert!(lines.is_empty(), "frame {frame}: {lines:?}");
            let t = coast_after(frame);
            let l = cx.world.launch_state(npc).expect("still launching");
            assert_eq!(l.coast_elapsed, t, "frame {frame}");
            assert!(!l.complete);
            let at = position(&cx, npc);
            assert_eq!(at, path_point(end, dir, t), "frame {frame}");
            // The launch orientation, as the body gives it back.
            let o = cx.world.get(npc).unwrap().engine.orientation;
            for (a, b) in o.m.iter().flatten().zip(orient.m.iter().flatten()) {
                assert!((a - b).abs() < 1e-6, "frame {frame}: {o:?}");
            }
            // Forward: along the launch orientation's negated third column.
            let moved = (e(at.x) - e(previous.x)) * e(-dir.x)
                + (e(at.y) - e(previous.y)) * e(-dir.y)
                + (e(at.z) - e(previous.z)) * e(-dir.z);
            if frame > 1 && t <= 3.0 {
                assert!(moved > 0.0, "frame {frame}: {moved}");
            }
            previous = at;
            assert!(!collisions(cx.world, npc));
            assert_eq!(exclusion(cx.world, npc), 7);
            continue;
        }
        // The frame after: `LaunchComplete` with the ring, once.
        assert_eq!(lines, [format!("LaunchComplete {ring_id} {npc_id}")]);
        break;
    }
    assert_eq!(frame, done_after + 1);
    assert_eq!(done_after, DONE_AFTER);
    assert!(!cx.world.is_launching(npc), "the action is removed");
    assert!(!has_controller(cx.world, npc, ControllerRef::Launch));
    assert!(collisions(cx.world, npc));
    assert_eq!(exclusion(cx.world, npc), 0);
    // The throttle went to 1 (twice), then the behaviour tick of the same
    // update, which the exclusion no longer holds back, flies the ship.
    // Pushed forward at 80 m/s (`AddToVelocity`), what the frame's PSI of
    // drag left of it, and nothing sideways.
    let v = cx.world.inspect(npc).unwrap().velocity;
    let m = cx.world.get(npc).unwrap().engine.orientation.m;
    let forward = -(e(v.x) * e(m[0][2]) + e(v.y) * e(m[1][2]) + e(v.z) * e(m[2][2]));
    let speed = (e(v.x) * e(v.x) + e(v.y) * e(v.y) + e(v.z) * e(v.z)).sqrt();
    assert!(forward > 60.0 && forward <= 80.0, "{v:?}");
    assert!(speed - forward < 1e-3, "{v:?}");
    // The launch invincibility is lifted (`Ship::slot_70`).
    assert_eq!(
        space_obj::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: false,
            protect: false,
            max_hp_loss: 1.0
        })
    );
    // The behaviour manager heard the throttle and the callback is gone.
    let m = cx.world.behavior_manager(npc).expect("made at the end");
    assert!(
        !m.state
            .directive_callbacks
            .contains(&LAUNCH_ACTION_CALLBACK)
    );
    world::physics_update(&mut cx, FRAME);
    assert!(heard().is_empty(), "reported once");
}

#[test]
fn an_npc_launches_from_a_station() {
    let Some((mut world, mut logic)) = setup("a station's launch") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let station = solar(&cx, b"he01_14");
    let station_id = id(&cx, station);
    assert_eq!(cx.world.get(station).unwrap().simple.type_bits, 0x100);
    // Content puts the ship on the berth's launch point with the launch
    // orientation, its `hpmount` on the point (as a player's launch does),
    // with the FIGHTER state graph.
    let (_, point, orient) = cx.world.launch_pos(station, 0);
    let npc = fighter(&mut cx, controller, point);
    let npc_id = id(&cx, npc);
    cx.world.beam_object(npc, point, orient);
    let mount = cx
        .world
        .hardpoint_os_world(npc, b"hpmount")
        .expect("hpmount");
    let start = Vector::new(
        point.x + (point.x - mount.x),
        point.y + (point.y - mount.y),
        point.z + (point.z - mount.z),
    );
    cx.world.beam_object(npc, start, orient);
    let graph = cx
        .world
        .data()
        .state_graphs
        .find(b"FIGHTER", GraphType::Leader);
    let op = BaseOp::new(Op::SetPersonalityParams(Box::new(SetPersonalityParams {
        state_graph: graph,
        content_callback: CallbackArg::Null,
        directive_callback: CallbackArg::Null,
        ..SetPersonalityParams::default()
    })));
    assert_eq!(ai::submit_state(&mut cx, npc_id, &op), OpRType::SUCCESS);
    let _ = heard();

    // `Start`'s AI launch: the callback registered, exclusion 6, the
    // launch directive {1, 0, 8} for the berth with terminal 1; the
    // collisions and the position stay, no controller.
    space_obj::launch(&mut cx, npc_id, station_id, 0).unwrap();
    let state = cx
        .world
        .launch_state(npc)
        .expect("the launch action")
        .clone();
    assert!(!state.is_physical() && state.directive);
    assert_eq!(
        (state.base, state.flags, state.launch_type),
        (station_id, 0x100, 0)
    );
    assert_eq!(exclusion(cx.world, npc), 6);
    assert!(collisions(cx.world, npc));
    assert!(!has_controller(cx.world, npc, ControllerRef::Launch));
    assert_eq!(position(&cx, npc), start);
    let m = cx.world.behavior_manager(npc).unwrap();
    assert_eq!(m.state.directive, [1, 0, ids::LAUNCH]);
    assert_eq!(m.state.behavior, ids::LAUNCH);
    assert!(
        m.state
            .directive_callbacks
            .contains(&LAUNCH_ACTION_CALLBACK)
    );
    assert_eq!(
        space_obj::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: true,
            protect: true,
            max_hp_loss: 0.0
        })
    );

    // Frames of 1/30 s (the server thread's period). The launch behaviour
    // waits 3.5 s, then flies out along the berth's points; the directive's
    // end (`NotifyDirectiveCallbacks`) calls `OnDirectiveComplete` in the
    // same frame: exclusion 0 and `LaunchComplete` with the station.
    let dt = 1.0 / 30.0;
    let mut frame = 0;
    let done = loop {
        frame += 1;
        assert!(frame < 70 * 30, "the launch ends within its 60 s limit");
        let results = cx
            .world
            .behavior_manager(npc)
            .unwrap()
            .state
            .completed
            .len();
        world::physics_update(&mut cx, dt);
        cx.world.update_global_time(dt);
        let lines = heard();
        let m = cx.world.behavior_manager(npc).unwrap();
        let ended = m.state.completed.len() > results;
        if !ended {
            assert!(lines.is_empty(), "frame {frame}: {lines:?}");
            assert_eq!(exclusion(cx.world, npc), 6);
            assert_eq!(m.state.behavior, ids::LAUNCH, "frame {frame}");
            continue;
        }
        assert_eq!(m.state.completed.last().unwrap().0, ids::LAUNCH);
        assert_ne!(m.state.behavior, ids::LAUNCH);
        assert_eq!(lines, [format!("LaunchComplete {station_id} {npc_id}")]);
        assert_eq!(exclusion(cx.world, npc), 0);
        assert!(cx.world.launch_state(npc).unwrap().complete);
        break frame;
    };
    // Not before the behaviour's 3.5 s wait, and by flying out, not by its
    // 60 s limit for an NPC.
    assert!(done > 105 && done < 60 * 30, "{done}");
    let moved = {
        let at = position(&cx, npc);
        let d = [
            e(at.x) - e(start.x),
            e(at.y) - e(start.y),
            e(at.z) - e(start.z),
        ];
        (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt()
    };
    // Out along the berth's points, some 200 m.
    assert!(moved > 150.0, "out of the berth: {moved}");
    assert_eq!(
        space_obj::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: false,
            protect: false,
            max_hp_loss: 1.0
        })
    );
    // The next update removes the action and clears its callback.
    world::physics_update(&mut cx, dt);
    assert!(heard().is_empty(), "reported once");
    assert!(!cx.world.is_launching(npc));
    let m = cx.world.behavior_manager(npc).unwrap();
    assert!(
        !m.state
            .directive_callbacks
            .contains(&LAUNCH_ACTION_CALLBACK)
    );
    assert!(collisions(cx.world, npc));
}

#[test]
fn the_bare_world_launches_without_a_server() {
    let Some((mut world, mut logic)) = setup("the bare world's launch") else {
        return;
    };
    let ring = {
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        world::load(&mut cx, he01());
        solar(&cx, b"Dock_Ring_he01_01")
    };
    let ring_id = world.get(ring).unwrap().simple.id;
    // A ship the world alone made, launched by `CShip::launch` itself.
    let start = Vector::new(-33000.0, 100.0, -28500.0);
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        start,
        Matrix::IDENTITY,
    );
    let ship = world.spawn_ship(he01(), &spawn).expect("the ship");
    let ship_id = world.get(ship).unwrap().simple.id;
    let (_, point, orient) = world.launch_pos(ring, 1);
    let (mount, _) = world.hardpoint_world(ship, b"hpmount").expect("hpmount");
    let end = path_end(point, &orient, start, mount);
    let dir = Vector::new(orient.m[0][2], orient.m[1][2], orient.m[2][2]);
    // An object that is not a ship does not launch. A base that is neither
    // a ring nor a station nor a capital ship (a fighter's type bits) makes
    // an action that launches nothing; the next launch ends it first (D720).
    world.launch(ring, ring, 0);
    assert!(!world.is_launching(ring));
    world.launch(ship, ship, 0);
    let state = world.launch_state(ship).expect("a ship is a base").clone();
    assert!(!state.is_physical() && state.flags & 0x20_0100 == 0);
    assert!(collisions(&world, ship));
    assert_eq!(exclusion(&world, ship), 0);
    world.launch(ship, ring, 3);
    let state = world.launch_state(ship).unwrap().clone();
    assert_eq!(
        (state.base, state.launch_type, state.position),
        (ring_id, 1, end)
    );
    assert!(!collisions(&world, ship));
    let _ = world.take_events();

    // `World::step` without hooks: the same path and the same frame.
    let mut frame = 0;
    loop {
        frame += 1;
        world.step(he01(), FRAME);
        let launched: Vec<_> = world
            .take_events()
            .into_iter()
            .filter(|e| matches!(e, WorldEvent::LaunchComplete { .. }))
            .collect();
        if frame <= DONE_AFTER {
            assert!(launched.is_empty(), "frame {frame}");
            let t = coast_after(frame);
            assert_eq!(world.launch_state(ship).unwrap().coast_elapsed, t);
            assert_eq!(position_of(&world, ship), path_point(end, dir, t));
            continue;
        }
        assert_eq!(
            launched,
            [WorldEvent::LaunchComplete {
                ship: ship_id,
                base: ring_id
            }]
        );
        break;
    }
    assert_eq!(frame, DONE_AFTER + 1);
    assert!(!world.is_launching(ship));
    assert!(collisions(&world, ship));
    assert_eq!(exclusion(&world, ship), 0);
    let m = world
        .behavior_manager(ship)
        .expect("`GetBehaviorManager`'s");
    assert!(
        !m.state
            .directive_callbacks
            .contains(&LAUNCH_ACTION_CALLBACK)
    );
    // No server: nothing was told.
    assert!(heard().is_empty());
}

fn position_of(world: &World, h: ObjHandle) -> Vector {
    world.get(h).unwrap().engine.position
}
