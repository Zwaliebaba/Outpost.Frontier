//! The trade-lane transit over the original data (`outdir/`, the rebranded
//! he01, whose lane runs from `he01_Trade_Lane_Ring_16` to ring 20): an
//! NPC's dock behaviour asks for the lane at ring 16 and SpaceSupport
//! answers (`Content/TradelaneRequests.cpp`); the ship enters the lane
//! (`Ship::go_tradelane` → `CShip::go_tradelane` and its `LaneAction`,
//! `Common/common_laneaction.cpp`), rides it ring by ring and leaves it
//! past the last ring (`LaneAction::stop` → `Ship::stop_tradelane`), and
//! the players who see the ship and the bus hear both ends
//! (`StarSystem::notify_go_tradelane`, `notify_stop_tradelane`). The
//! instant trade lane's behaviour ends with the server's type-4 event, a
//! request at an object that is not a ring is refused, and the bare world
//! leaves a lane on request. Expected values follow the C++ logic.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, ControllerKind, CreateParams, Cx, Logic};
use game_logic::{dock, ship, world};
use game_sim::ai::behavior::{DirectiveOp, DockOp, InstantTradeLaneOp, ids};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::ship::{Lane, LaneRing};
use game_sim::object::world::{Assets, ControllerRef, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::math::{
    Matrix, Transform, Vector, matrix_to_quaternion, quat_slerp, quaternion_to_matrix,
};
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

/// A client sink that keeps what it is sent.
#[derive(Clone, Default)]
struct Shared {
    sent: Rc<RefCell<Vec<(u32, ToClient)>>>,
}

impl Shared {
    fn take(&self) -> Vec<(u32, ToClient)> {
        std::mem::take(&mut *self.sent.borrow_mut())
    }
}

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.sent.borrow_mut().push((client, message));
    }
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(what: &str, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, true);
    logic.deliver_hook = Some(|_, _, kind, message| {
        let line = match message {
            ControllerMessage::TradelaneRequest {
                ship,
                ring,
                end_ring,
                system,
            } => format!("TradelaneRequest {ship} {ring} {end_ring} {system}"),
            ControllerMessage::TradelaneRelease { ship, ring, system } => {
                format!("TradelaneRelease {ship} {ring} {system}")
            }
            ControllerMessage::TradelaneEnter {
                ship,
                entrance,
                exit,
                system,
            } => format!("TradelaneEnter {ship} {entrance} {exit} {system}"),
            ControllerMessage::TradelaneExit {
                ship,
                entrance,
                exit,
                system,
            } => format!("TradelaneExit {ship} {entrance} {exit} {system}"),
            _ => return,
        };
        // Each message once (the broadcast reaches every controller).
        if kind == ControllerKind::SpaceSupport {
            HEARD.with(|h| h.borrow_mut().push(line));
        }
    });
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// The frame of the server thread (1/30 s): one PSI of the frame's
/// length on a multiplayer server.
const FRAME: f32 = 1.0 / 30.0;

/// he01 loaded with Content's GameSupport (the NPCs' controller) and
/// SpaceSupport (the lane requests' answers), and players 1 and 2 in it,
/// each with a parked ship: player 1's near ring 16, player 2's near ring
/// 20 (the lane's ends), so that the multiplayer frame steps the physics
/// and each sees one end of the lane.
fn he01_with_players(cx: &mut Cx<'_>) -> u32 {
    let controller = neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0;
    let _ = neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"SpaceSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Normal,
    );
    // Each constructor drew its heartbeat's phase (P3.4a). The oracle's
    // harness seeds again after the controllers exist (design/reference-build.md
    // section 10, start-up steps 3 to 5), so the transit draws from the
    // generator as it was before them.
    *cx.world.rng() = neuron_core::rng::CrtRand::default();
    world::load(cx, he01());
    let parked = [
        (1, Vector::new(4400.0, 600.0, -50000.0)),
        (2, Vector::new(-11163.0, 600.0, -79469.0)),
    ];
    for (session, at) in parked {
        let mut spawn = ShipSpawn::new(
            create_id(b"he_fighter"),
            create_id(b"li_p_he_fighter_loadout01"),
            at,
            Matrix::IDENTITY,
        );
        spawn.owner_player = session;
        let ship = cx
            .world
            .spawn_ship(he01(), &spawn)
            .expect("a player's ship");
        let ship_id = id(cx, ship);
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = he01();
        slot.ship_object_id = ship_id;
        world::add_player(cx, he01(), session);
    }
    controller
}

fn ring(cx: &Cx<'_>, n: u32) -> ObjHandle {
    let name = format!("he01_Trade_Lane_Ring_{n}");
    cx.world.find(create_id(name.as_bytes())).expect("the ring")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

/// An NPC fighter made through `pub::SpaceObj::Create` at `at`, with the
/// FIGHTER state graph.
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
    let h = cx.world.find(id).unwrap();
    let graph = cx
        .world
        .data()
        .state_graphs
        .find(b"FIGHTER", GraphType::Leader);
    assert_eq!(cx.world.submit_state(h, graph, None), 0);
    h
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

fn frame(cx: &mut Cx<'_>) {
    world::physics_update(cx, FRAME);
    cx.world.update_global_time(FRAME);
}

fn lane_packets(sent: &[(u32, ToClient)]) -> Vec<String> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(CommonMessage::GoTradeLane(x)) => Some(format!(
                "{c}: GOTRADELANE {} {} {} {}",
                x.object_id, x.tradelane_ring1, x.tradelane_ring2, x.field_0c
            )),
            ToClient::Common(CommonMessage::StopTradeLane(x)) => Some(format!(
                "{c}: STOPTRADELANE {} {} {}",
                x.ship_id, x.tradelane_ring1, x.tradelane_ring2
            )),
            _ => None,
        })
        .collect()
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// A forward lane's ring frame: the ring's `HpRightLane` hardpoint
/// (`LaneAction::BuildRingTransform` with `bHpLeftLane` set).
fn right_lane(cx: &Cx<'_>, ring: ObjHandle) -> LaneRing {
    let (position, orientation) = cx
        .world
        .hardpoint_world(ring, b"HpRightLane")
        .expect("HpRightLane");
    LaneRing {
        ring: Some(id(cx, ring)),
        orientation,
        position,
    }
}

/// `InitializeApproach`'s speed fraction: the squared speed's float image
/// seeds the `0x5f3759df` guess, one Newton step, the speed over 2500,
/// rounded once.
fn speed_fraction(v: Vector) -> f32 {
    let sq = (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z);
    let bits = i32::from_ne_bytes(r(sq).to_bits().to_ne_bytes());
    let guess_bits = 0x5f37_59df_i32.wrapping_sub(bits >> 1);
    let guess = e(f32::from_bits(u32::from_ne_bytes(guess_bits.to_ne_bytes())));
    let refine = 1.5 - ((guess * guess) * sq) * 0.5;
    r(((sq / 2500.0) * refine) * guess)
}

/// What one PSI of `LaneAction::do_simulation_controller` makes of a
/// forward lane of an NPC without leaders whose rings are whole, from the
/// action before it and the ship's position and orientation (the
/// reference's `WorldPosition`, synced after the step): the action after
/// it and the pose the ship is beamed to (none for the PSI that ends the
/// lane).
struct Psi {
    lane: Lane,
    beamed: Option<Transform>,
}

fn lane_psi(cx: &Cx<'_>, before: &Lane, at: Vector, orient: Matrix) -> Psi {
    let mut l = before.clone();
    l.elapsed = r(e(l.elapsed) + e(FRAME));
    let mut progress = 0.0f32;
    if l.exit_state == 4 {
        l.pose = Transform::new(orient, at);
        if l.exit_ramp_start == 0.0 {
            l.exit_ramp_start = l.ramp;
        }
        let ramp = e(l.ramp) - (e(l.exit_ramp_start) - e(l.ramp_target) * 0.4) * e(FRAME);
        l.ramp = r(ramp);
        if ramp <= e(l.ramp_target) {
            l.ramp = l.ramp_target;
            l.exit_state = 0;
            return Psi {
                lane: l,
                beamed: None,
            };
        }
    } else {
        // `ProjectOntoSegment`.
        let (a, b) = (l.entrance.position, l.exit.position);
        let d = Vector::new(r(e(b.x) - e(a.x)), r(e(b.y) - e(a.y)), r(e(b.z) - e(a.z)));
        let len = ((e(d.z) * e(d.z) + e(d.y) * e(d.y)) + e(d.x) * e(d.x)).sqrt();
        let inv = 1.0 / len;
        let dir = Vector::new(r(e(d.x) * inv), r(e(d.y) * inv), r(e(d.z) * inv));
        let rel = Vector::new(
            r(e(at.x) - e(a.x)),
            r(e(at.y) - e(a.y)),
            r(e(at.z) - e(a.z)),
        );
        let dot = (e(dir.z) * e(rel.z) + e(dir.y) * e(rel.y)) + e(rel.x) * e(dir.x);
        let dd = (e(dir.z) * e(dir.z) + e(dir.y) * e(dir.y)) + e(dir.x) * e(dir.x);
        let along = dot / dd;
        progress = r(along / e(r(len)));
        let sx = r(e(dir.x) * along);
        let sy = r(e(dir.y) * along);
        let sz = r(e(dir.z) * along);
        l.pose = Transform::new(
            l.entrance.orientation,
            Vector::new(r(e(sx) + e(a.x)), r(e(sy) + e(a.y)), r(e(sz) + e(a.z))),
        );
        if progress >= 1.0 {
            // The next ring (`traverse_rings` forward, `FindById`).
            let exit = cx.world.find(l.exit.ring.unwrap()).unwrap();
            let next = cx.world.traverse_rings(exit, true).unwrap();
            l.entrance = l.exit;
            if next != 0 {
                let ring = cx.world.find(next).unwrap();
                l.exit = right_lane(cx, ring);
                progress = r(e(progress) % 1.0);
                if l.exit_state != 3 && cx.world.traverse_rings(ring, true).unwrap() == 0 {
                    l.exit_state = 3;
                }
            } else {
                l.exit.ring = None;
                l.exit_state = 4;
            }
        }
    }
    match l.exit_state {
        1 => {
            let ramped = e(FRAME) * 0.125 + e(l.ramp);
            l.ramp = r(ramped);
            if ramped >= 1.0 {
                l.exit_state = 2;
                l.ramp = 1.0;
            }
        }
        2 => l.ramp = 1.0,
        3 => l.ramp = r(1.0 - (1.0 - 400.0 / 2500.0) * e(progress)),
        _ => {}
    }
    let speed = r(2500.0 * e(l.ramp));
    let advance = e(speed) * e(FRAME);
    let m = l.pose.rotation.m;
    let dx = r(-e(m[0][2]) * advance);
    let dy = r(-e(m[1][2]) * advance);
    let dz = -e(m[2][2]) * advance;
    let t = l.pose.translation;
    l.pose.translation = Vector::new(r(e(dx) + e(t.x)), r(e(dy) + e(t.y)), r(dz + e(t.z)));
    let c = l.captured_position;
    l.captured_position = Vector::new(r(e(dx) + e(c.x)), r(e(dy) + e(c.y)), r(dz + e(c.z)));
    if l.exit_state == 1 && e(l.elapsed) < 3.0 {
        let q = matrix_to_quaternion(&l.pose.rotation);
        let blend = r(e(l.elapsed) * (1.0 / 3.0));
        l.pose.rotation = quaternion_to_matrix(&quat_slerp(&l.captured_orientation, &q, blend));
        let keep = 1.0 - e(blend);
        let (t, c) = (l.pose.translation, l.captured_position);
        l.pose.translation = Vector::new(
            r(keep * e(c.x) + e(blend) * e(t.x)),
            r(e(r(keep * e(c.y))) + e(blend) * e(t.y)),
            r(e(r(keep * e(c.z))) + e(r(e(blend) * e(t.z)))),
        );
    }
    Psi {
        beamed: Some(l.pose),
        lane: l,
    }
}

/// The ship after its PSI: beamed to the pose at rest, then pushed by the
/// engine and turned by the steering, whose controllers the PSI runs after
/// the lane's (IVP runs a core's controllers from the last registered), so
/// the ship ends the PSI one PSI of that velocity past the pose, with
/// about the pose's orientation.
fn after_beam(
    world: &World,
    h: ObjHandle,
    beamed: &Transform,
    at: Vector,
    orient: &Matrix,
) -> bool {
    let v = world.velocity(h);
    let t = beamed.translation;
    let expected = [
        e(t.x) + e(v.x) * e(FRAME),
        e(t.y) + e(v.y) * e(FRAME),
        e(t.z) + e(v.z) * e(FRAME),
    ];
    let off = [
        e(at.x) - expected[0],
        e(at.y) - expected[1],
        e(at.z) - expected[2],
    ];
    let turned = orient
        .m
        .iter()
        .flatten()
        .zip(beamed.rotation.m.iter().flatten())
        .all(|(x, y)| (x - y).abs() < 1e-3);
    off.iter().all(|d| d.abs() < 2e-3) && turned
}

/// The frames the test saw a lane's events in.
#[derive(Default)]
struct Seen {
    request: Option<usize>,
    enter: Option<usize>,
    full_speed: Option<usize>,
    hops: Vec<(usize, Option<u32>)>,
    last_leg: Option<usize>,
    leaving: Option<usize>,
    exit: Option<usize>,
    removed: Option<usize>,
}

#[test]
fn an_npc_rides_the_he01_lane_from_ring_16_to_ring_20() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("trade lane", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_players(&mut cx);
    let rings: Vec<ObjHandle> = (16..=20).map(|n| ring(&cx, n)).collect();
    let ring_ids: Vec<u32> = rings.iter().map(|&h| id(&cx, h)).collect();
    for (n, &h) in rings.iter().enumerate() {
        assert!(cx.world.is_lane_ring(h));
        assert_eq!(cx.world.get(h).unwrap().simple.type_bits, 0x80);
        let next = ring_ids.get(n + 1).copied().unwrap_or(0);
        let prev = if n > 0 { ring_ids[n - 1] } else { 0 };
        assert_eq!(cx.world.traverse_rings(h, true), Some(next));
        assert_eq!(cx.world.traverse_rings(h, false), Some(prev));
    }
    let he01_14 = cx.world.find(create_id(b"he01_14")).unwrap();
    assert_eq!(cx.world.traverse_rings(he01_14, true), None);

    // The NPC traffic's way into a lane (the `dock_trade_lane` scenario): a
    // fighter 1500 m from ring 16 told to dock with it.
    let npc = fighter(&mut cx, controller, Vector::new(4378.0, 0.0, -51069.0));
    let npc_id = id(&cx, npc);
    // `get_lane_start` from the fighter: ring 16 is the lane's first ring,
    // so the lane ring is ring 17, the start its right lane, the direction
    // the unit vector from ring 16 to ring 17.
    let start = cx.world.get_lane_start(npc, Some(rings[0]), None);
    assert_eq!(start.result, 0);
    assert_eq!(start.lane, Some(rings[1]));
    assert_eq!(start.start, Some(right_lane(&cx, rings[0]).position));
    let (p16, p17) = (position(&cx, rings[0]), position(&cx, rings[1]));
    let d = Vector::new(p17.x - p16.x, p17.y - p16.y, p17.z - p16.z);
    let len = ((e(d.z) * e(d.z) + e(d.y) * e(d.y)) + e(d.x) * e(d.x)).sqrt();
    let inv = 1.0 / len;
    let unit = Vector::new(r(e(d.x) * inv), r(e(d.y) * inv), r(e(d.z) * inv));
    assert_eq!(start.direction, Some(unit));
    // From ring 17 the fighter, behind it, goes on to ring 18; from ring
    // 20, which has no next ring, the lane runs the other way: ring 19 and
    // the left lane's start.
    let on = cx.world.get_lane_start(npc, Some(rings[1]), None);
    assert_eq!((on.result, on.lane), (0, Some(rings[2])));
    let back = cx.world.get_lane_start(npc, Some(rings[4]), None);
    assert_eq!((back.result, back.lane), (0, Some(rings[3])));
    let left = cx.world.hardpoint_world(rings[4], b"HpLeftLane").unwrap().0;
    assert_eq!(back.start, Some(left));
    let op = DirectiveOp::Dock(DockOp {
        dock_object: ring_ids[0],
        ..DockOp::default()
    });
    cx.world.submit_directive(npc, &DirectiveOp::Cancel);
    cx.world.submit_directive(npc, &op);
    let _ = (heard(), sink.take());

    let mut seen = Seen::default();
    let mut before: Option<Lane> = None;
    let mut pose = (
        position(&cx, npc),
        cx.world.get(npc).unwrap().engine.orientation,
    );
    let mut velocity = cx.world.velocity(npc);
    for f in 1..=1300usize {
        frame(&mut cx);
        let lines = heard();
        let packets = lane_packets(&sink.take());
        let now = cx.world.lane_state(npc).cloned();
        let at = position(&cx, npc);
        let orient = cx.world.get(npc).unwrap().engine.orientation;
        // The request: type 2 at ring 16 for ring 17, answered at once.
        if lines.iter().any(|l| l.starts_with("TradelaneRequest")) {
            assert_eq!(seen.request, None);
            seen.request = Some(f);
            assert_eq!(
                lines,
                [format!(
                    "TradelaneRequest {npc_id} {} {} {}",
                    ring_ids[0],
                    ring_ids[1],
                    he01()
                )]
            );
            // SpaceSupport granted it (4) to the dock behaviour's
            // callback (`OnResult`): the ring's spline (state 6, 16 s);
            // the behaviour lets the request go.
            let s = ship::ship_state(&mut cx, npc).unwrap();
            assert!(!s.request_busy && !s.request_callback);
            let m = cx.world.behavior_manager(npc).unwrap();
            let dock = m
                .behavior(ids::DOCK)
                .and_then(|b| b.as_any())
                .and_then(|b| b.downcast_ref::<game_sim::ai::dock::DockBehavior>())
                .unwrap();
            assert_eq!((dock.mode, dock.field_408), (6, 16.0));
        }
        match (&before, &now) {
            (None, Some(l)) => {
                // The hand-off: `go_tradelane(16, 17, false, 0)` in the
                // behaviour tick, the first PSI in the frame's step.
                seen.enter = Some(f);
                assert_eq!(
                    lines,
                    [format!(
                        "TradelaneEnter {npc_id} {} {} {}",
                        ring_ids[0],
                        ring_ids[1],
                        he01()
                    )]
                );
                // Player 1 sees the ship at ring 16; player 2 does not.
                assert_eq!(
                    packets,
                    [format!(
                        "1: GOTRADELANE {npc_id} {} {} 0",
                        ring_ids[0], ring_ids[1]
                    )]
                );
                assert_eq!((l.entrance_ring, l.exit_ring), (ring_ids[0], ring_ids[1]));
                assert!(l.forward && !l.fast_exit && l.speed == 0.0);
                let ramp0 = speed_fraction(velocity);
                assert_eq!(l.ramp_target, ramp0);
                assert!(ramp0 > 0.0 && ramp0 < 0.05, "{ramp0}");
                let entered = Lane {
                    entrance: right_lane(&cx, rings[0]),
                    exit: right_lane(&cx, rings[1]),
                    exit_state: 1,
                    ramp: ramp0,
                    elapsed: 0.0,
                    exit_ramp_start: 0.0,
                    captured_position: pose.0,
                    captured_orientation: matrix_to_quaternion(&pose.1),
                    ..l.clone()
                };
                let psi = lane_psi(&cx, &entered, pose.0, pose.1);
                let beamed = psi.beamed.unwrap();
                assert_eq!(l.pose, beamed, "frame {f}");
                assert!(after_beam(cx.world, npc, &beamed, at, &orient), "frame {f}");
                assert_eq!(l.ramp, psi.lane.ramp);
                assert_eq!(l.exit_state, 1);
                assert!(cx.world.is_using_tradelane(npc));
                assert!(!collisions(cx.world, npc));
                assert_eq!(exclusion(cx.world, npc), 3);
                assert!(has_controller(cx.world, npc, ControllerRef::Lane));
            }
            (Some(b), Some(l)) if l.exit_state != 0 => {
                // In the lane: each frame's PSI as the C++ computes it.
                let psi = lane_psi(&cx, b, pose.0, pose.1);
                let beamed = psi.beamed.expect("in the lane");
                assert_eq!(l.pose, beamed, "frame {f}");
                assert!(after_beam(cx.world, npc, &beamed, at, &orient), "frame {f}");
                assert_eq!(l.exit_state, psi.lane.exit_state, "frame {f}");
                assert_eq!(l.ramp, psi.lane.ramp, "frame {f}");
                assert_eq!(l.exit.ring, psi.lane.exit.ring, "frame {f}");
                assert_eq!(l.entrance, psi.lane.entrance, "frame {f}");
                assert_eq!(l.elapsed, psi.lane.elapsed, "frame {f}");
                let speed = cx
                    .world
                    .get(npc)
                    .unwrap()
                    .as_ship()
                    .unwrap()
                    .tradelane_speed;
                assert_eq!(speed, r(2500.0 * e(l.ramp)), "frame {f}");
                assert_eq!(cx.world.tradelane_speed(npc), speed);
                if b.exit.ring != l.exit.ring {
                    seen.hops.push((f, l.exit.ring));
                }
                if b.exit_state == 1 && l.exit_state == 2 {
                    seen.full_speed = Some(f);
                }
                if b.exit_state != 3 && l.exit_state == 3 {
                    seen.last_leg = Some(f);
                }
                if b.exit_state != 4 && l.exit_state == 4 {
                    seen.leaving = Some(f);
                }
                assert!(lines.is_empty() && packets.is_empty(), "frame {f}");
                assert_eq!(exclusion(cx.world, npc), 3);
            }
            (Some(b), Some(l)) if b.exit_state != 0 => {
                // The end: the ramp reached its target in the PSI, which
                // stopped the action (`stop(0)`) and called
                // `Ship::stop_tradelane`: the bus and player 2, who sees the
                // ship at ring 20, hear it. The exit ring the C++ reports
                // is the last leg's first ring (ring 20).
                seen.exit = Some(f);
                let psi = lane_psi(&cx, b, pose.0, pose.1);
                assert!(psi.beamed.is_none());
                assert_eq!(psi.lane.ramp, l.ramp);
                assert_eq!(l.ramp, l.ramp_target);
                assert_eq!(l.entrance.ring, Some(ring_ids[4]));
                assert_eq!(
                    lines,
                    [format!(
                        "TradelaneExit {npc_id} {} {} {}",
                        ring_ids[0],
                        ring_ids[4],
                        he01()
                    )]
                );
                assert_eq!(
                    packets,
                    [format!(
                        "2: STOPTRADELANE {npc_id} {} {}",
                        ring_ids[0], ring_ids[4]
                    )]
                );
                assert!(!cx.world.is_using_tradelane(npc));
                assert_eq!(cx.world.tradelane_speed(npc), 0.0);
                assert!(collisions(cx.world, npc));
                assert_eq!(exclusion(cx.world, npc), 0);
                assert!(!has_controller(cx.world, npc, ControllerRef::Lane));
                let throttle = cx
                    .world
                    .get(npc)
                    .unwrap()
                    .as_ship()
                    .unwrap()
                    .flight
                    .throttle;
                assert_eq!(throttle, 1.0);
            }
            (Some(_), None) => {
                // The ship's next update removes the stopped action.
                seen.removed = Some(f);
            }
            _ => {}
        }
        before = now;
        pose = (at, orient);
        velocity = cx.world.velocity(npc);
    }
    // The timings the oracle's `dock_trade_lane` shows
    // (design/reference-build.md): the request at frame 222, the lane at
    // frame 588 and its end at frame 1188.
    assert_eq!(seen.request, Some(222));
    assert_eq!(seen.enter, Some(588));
    assert_eq!(seen.exit, Some(1188));
    assert_eq!(seen.removed, Some(1189));
    // The hops at rings 17, 18 and 19 (the last leg, from ring 19 to ring
    // 20), then past ring 20 out of the lane; full speed (state 2) between
    // rings 17 and 18.
    let hops: Vec<Option<u32>> = seen.hops.iter().map(|&(_, r)| r).collect();
    assert_eq!(
        hops,
        [
            Some(ring_ids[2]),
            Some(ring_ids[3]),
            Some(ring_ids[4]),
            None
        ]
    );
    assert_eq!(seen.last_leg, Some(seen.hops[2].0));
    assert_eq!(seen.leaving, Some(seen.hops[3].0));
    let full = seen.full_speed.expect("full speed");
    assert!(seen.hops[0].0 < full && full < seen.hops[1].0, "{full}");
    println!(
        "request {:?}, lane {:?}, full speed {full}, hops {:?}, end {:?}",
        seen.request, seen.enter, seen.hops, seen.exit
    );
}

#[test]
fn the_instant_lane_ends_with_the_servers_event() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("instant trade lane", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_players(&mut cx);
    let (r19, r20) = (ring(&cx, 19), ring(&cx, 20));
    let (r19_id, r20_id) = (id(&cx, r19), id(&cx, r20));
    // 300 m before ring 19, the instant lane from ring 19 to ring 20 (the
    // `trade_lane` scenario's set-up on the lane's last leg).
    let npc = fighter(&mut cx, controller, Vector::new(-7316.0, 0.0, -71679.0));
    let npc_id = id(&cx, npc);
    let op = DirectiveOp::InstantTradeLane(InstantTradeLaneOp {
        entrance: r19_id,
        exit: r20_id,
        speed: 0.0,
    });
    cx.world.submit_directive(npc, &DirectiveOp::Cancel);
    cx.world.submit_directive(npc, &op);
    let _ = (heard(), sink.take());
    let mut entered = None;
    let mut ended = None;
    let mut done = None;
    for f in 1..=400usize {
        frame(&mut cx);
        let lines = heard();
        let packets = lane_packets(&sink.take());
        if entered.is_none()
            && let Some(l) = cx.world.lane_state(npc)
        {
            entered = Some(f);
            // A fast entry onto the last leg: state 3, full speed, the
            // blend's 3 s already run, the leaving target 80/2500.
            assert!(l.fast_exit && l.forward);
            assert_eq!((l.entrance_ring, l.exit_ring), (r19_id, r20_id));
            assert_eq!(l.exit_state, 3);
            assert_eq!(l.ramp_target, r(80.0 / 2500.0));
            assert_eq!(l.elapsed, r(3.0 + e(FRAME)));
            assert_eq!(
                lines,
                [format!(
                    "TradelaneEnter {npc_id} {r19_id} {r20_id} {}",
                    he01()
                )]
            );
            assert!(packets.is_empty(), "nobody sees ring 19: {packets:?}");
            continue;
        }
        if lines.iter().any(|l| l.starts_with("TradelaneExit")) {
            ended = Some(f);
            // `notify_stop_tradelane`'s type-4 event reaches the ship's own
            // instant trade-lane behaviour (`HandleTargetEvent`): state 5
            // and the lane's request released at once (`slot_60(2, &19,
            // 0)`, SpaceSupport's `HandleTradelaneRelease`).
            assert_eq!(
                lines,
                [
                    format!("TradelaneExit {npc_id} {r19_id} {r20_id} {}", he01()),
                    format!("TradelaneRelease {npc_id} {r19_id} {}", he01()),
                ]
            );
            assert_eq!(
                packets,
                [format!("2: STOPTRADELANE {npc_id} {r19_id} {r20_id}")]
            );
            let m = cx.world.behavior_manager(npc).unwrap();
            assert_eq!(m.state.behavior, ids::INSTANT_TRADE_LANE);
            let itl = m
                .behavior(ids::INSTANT_TRADE_LANE)
                .and_then(|b| b.as_any())
                .and_then(|b| b.downcast_ref::<game_sim::ai::itl::InstantTradeLaneBehavior>())
                .unwrap();
            assert_eq!(itl.state, 5);
            continue;
        }
        if let Some(end) = ended {
            // The behaviour's next tick (the exclusion lifted, within the
            // manager's 0.2 s) ends the directive with result 0.
            let m = cx.world.behavior_manager(npc).unwrap();
            if m.state.completed.contains(&(ids::INSTANT_TRADE_LANE, 0)) {
                assert_eq!(m.state.directive[0], 0);
                assert!(f <= end + 7, "{f} {end}");
                done = Some(f);
                break;
            }
        }
    }
    assert!(
        entered.is_some() && ended.is_some(),
        "{entered:?} {ended:?}"
    );
    assert!(done.is_some(), "the directive ended");
}

#[test]
fn an_instant_lane_past_the_next_ring_runs_the_left_lane() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the trade_lane scenario's lane", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_players(&mut cx);
    let (r16, r17, r18) = (ring(&cx, 16), ring(&cx, 17), ring(&cx, 18));
    // The `trade_lane` scenario: 300 m before ring 16, the instant lane
    // from ring 16 to ring 18.
    let npc = fighter(&mut cx, controller, Vector::new(3778.0, 0.0, -52104.0));
    let op = DirectiveOp::InstantTradeLane(InstantTradeLaneOp {
        entrance: id(&cx, r16),
        exit: id(&cx, r18),
        speed: 0.0,
    });
    cx.world.submit_directive(npc, &DirectiveOp::Cancel);
    cx.world.submit_directive(npc, &op);
    let mut entered = None;
    for f in 1..=60usize {
        frame(&mut cx);
        if cx.world.lane_state(npc).is_some() {
            entered = Some(f);
            break;
        }
    }
    // `go_tradelane` at frame 12 (design/reference-build.md). The ring
    // after ring 16 is ring 17, not the exit ring, so the action runs the
    // rings' other way (`bHpLeftLane` clear): each ring's frame is its
    // `HpLeftLane` with the right lane's orientation turned by `RotY(π)`.
    assert_eq!(entered, Some(12));
    let l = cx.world.lane_state(npc).unwrap();
    assert!(!l.forward && l.fast_exit);
    assert_eq!(l.exit_state, 2, "ring 18 has a ring before it");
    let (left, _) = cx.world.hardpoint_world(r16, b"HpLeftLane").unwrap();
    let (_, right) = cx.world.hardpoint_world(r16, b"HpRightLane").unwrap();
    assert_eq!(l.entrance.position, left);
    let (c, s) = (
        f64::from(core::f32::consts::PI).cos() as f32,
        f64::from(core::f32::consts::PI).sin() as f32,
    );
    let turn = Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]);
    assert_eq!(
        l.entrance.orientation,
        neuron_core::math::mul_matrix(&turn, &right)
    );
    assert_eq!(cx.world.traverse_rings(r16, true), Some(id(&cx, r17)));
}

#[test]
fn a_lane_request_at_a_station_is_refused() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a refused lane", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_players(&mut cx);
    let station = cx.world.find(create_id(b"he01_14")).unwrap();
    let station_id = id(&cx, station);
    let r16_id = id(&cx, ring(&cx, 16));
    let npc = fighter(&mut cx, controller, Vector::new(4378.0, 0.0, -51069.0));
    let npc_id = id(&cx, npc);
    let _ = (heard(), sink.take());
    let returned = |sent: Vec<(u32, ToClient)>| -> Vec<(u32, u32, u8, u8)> {
        sent.into_iter()
            .filter_map(|(c, m)| match m {
                ToClient::Server(ServerMessage::RequestReturned(x)) => {
                    Some((c, x.object_id, x.request_type, x.response))
                }
                _ => None,
            })
            .collect()
    };
    // `slot_5c(2, station, 0, 0)` without a callback: SpaceSupport answers
    // 5 (not a lane ring) and activates nothing; the answer goes to the
    // ship's owner (0, an NPC) and the ship's request ends.
    assert_eq!(dock::request(&mut cx, npc, 2, station_id, 0, 0, false), 0);
    assert_eq!(
        heard(),
        [format!(
            "TradelaneRequest {npc_id} {station_id} 0 {}",
            he01()
        )]
    );
    assert_eq!(returned(sink.take()), [(0, npc_id, 2, 5)]);
    assert!(!ship::ship_state(&mut cx, npc).unwrap().request_busy);
    // The same at a ring: granted (4).
    assert_eq!(dock::request(&mut cx, npc, 2, r16_id, 0, 0, false), 0);
    let _ = heard();
    assert_eq!(returned(sink.take()), [(0, npc_id, 2, 4)]);
    // A ship that is not in a lane: the go_tradelane notices find no lane.
    assert!(!cx.world.is_using_tradelane(npc));
    assert_eq!(cx.world.tradelane_entrance_ring(npc), 0);
}

#[test]
fn the_bare_world_leaves_a_lane_on_request() {
    let sink = Shared::default();
    let Some((mut world, _logic)) = setup("the bare world's lane", &sink) else {
        return;
    };
    world.load_system(he01());
    let spawn_ring = |world: &mut World, n: u32| {
        let name = format!("he01_Trade_Lane_Ring_{n}");
        let sp = world
            .system_solar_spawns(he01())
            .into_iter()
            .find(|s| s.nickname == create_id(name.as_bytes()))
            .unwrap();
        world.spawn_solar(he01(), &sp).unwrap()
    };
    let (r16, r17) = (spawn_ring(&mut world, 16), spawn_ring(&mut world, 17));
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(3778.0, 0.0, -52104.0),
        Matrix::IDENTITY,
    );
    let ship = world.spawn_ship(he01(), &spawn).unwrap();
    // A missing ring is refused (the C++ reads it through null).
    assert_eq!(world.go_tradelane(ship, Some(r16), None, true, 0.0), -5);
    assert_eq!(world.go_tradelane(ship, Some(r16), Some(r17), true, 0.0), 0);
    assert!(world.is_using_tradelane(ship));
    assert_eq!(world.lane_state(ship).unwrap().exit_state, 2);
    for _ in 0..10 {
        world.step(he01(), FRAME);
    }
    assert_eq!(world.tradelane_speed(ship), 2500.0);
    // `request_exit_tradelane`: state 4 with the turn, at -0.56 rad/s about
    // the ship's Y, the speed falling from 1 to 80/2500 at 0.9872/s.
    world.request_exit_tradelane(ship);
    let l = world.lane_state(ship).unwrap();
    assert!(l.exit_flag && l.exit_state == 4);
    let mut frames = 0;
    while world.lane_state(ship).is_some_and(|l| l.exit_state != 0) {
        world.step(he01(), FRAME);
        frames += 1;
        assert!(frames < 100);
    }
    // 1 - 0.9872 t falls to 0.032 after 0.98 s: the 30th PSI.
    assert_eq!(frames, 30);
    // Without a server the traveller's `stop_tradelane` is `CShip`'s.
    assert!(!world.is_using_tradelane(ship));
    assert!(world.lane_state(ship).is_some(), "until the next update");
    world.step(he01(), FRAME);
    assert!(world.lane_state(ship).is_none());
}
