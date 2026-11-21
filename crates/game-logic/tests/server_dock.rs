//! The server's docking and launching over the original data (`outdir/`,
//! the rebranded he01 and he02): a dock request answered and queued
//! (`Ship::slot_5c`, `pub::SpaceObj::Dock`, `request_returned`), the dock
//! queue's tick taking a ship in (`EqObj::process_dock_queue`: a station's
//! landing, a gate's and a jump hole's `complete_dock` with the death
//! sequence that follows), `pub::SpaceObj::InstantDock`, the AI's request
//! through the server's hooks, and a player's launch from a base
//! (`IServerImpl::RequestCreateShip`, `PlayerLaunch`,
//! `Server_GetLaunchPosition`). Expected values follow the C++
//! (`Server/server_gameobject.cpp`, `server_classes.cpp`,
//! `IServerImpl.cpp`, `server_pub.cpp`).

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::PubError;
use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::character_file::CollisionGroupDesc;
use game_logic::dock::{self, DockRequest};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::ship;
use game_logic::world;
use game_sim::ai::behavior::{DirectiveOp, DockOp};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector, quaternion_to_matrix, transform_matrix};
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

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

fn setup(what: &str, config: WorldConfig, sink: &Shared) -> Option<(World, Logic)> {
    let data = data(what)?;
    let world = World::new(data, config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::DockRequest {
                ship,
                target,
                system,
                flag,
            } => format!("DockRequest {ship} {target} {system} {flag}"),
            ControllerMessage::DockCancel { ship, target, arg } => {
                format!("DockCancel {ship} {target} {arg}")
            }
            ControllerMessage::DockComplete { ship, dock, system } => {
                format!("DockComplete {ship} {dock} {system}")
            }
            ControllerMessage::Msg0x27 {
                ship,
                base,
                target,
                land,
            } => format!("Msg0x27 {ship} {base} {target} {land}"),
            ControllerMessage::Msg0x25 {
                ship,
                death_sequence,
                system,
            } => format!("Msg0x25 {ship} {death_sequence} {system}"),
            ControllerMessage::PlayerLaunch { player, base } => {
                format!("PlayerLaunch {player} {base}")
            }
            ControllerMessage::FreeTerminalRequest { player, target, .. } => {
                format!("FreeTerminalRequest {player} {target}")
            }
            ControllerMessage::SpaceEnter {
                system,
                player,
                launch_object,
                launch_kind,
            } => format!("SpaceEnter {system} {player} {launch_object} {launch_kind}"),
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
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

fn solar(cx: &Cx<'_>, nickname: &[u8]) -> ObjHandle {
    cx.world.find(create_id(nickname)).expect("the solar")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

/// An NPC fighter made through `pub::SpaceObj::Create` at `at`.
fn fighter(cx: &mut Cx<'_>, controller: u32, system: u32, at: Vector) -> ObjHandle {
    let info = ShipInfo::new(
        controller,
        system,
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    let id = space_obj::create(cx, &info).expect("the ship is made");
    cx.world.find(id).unwrap()
}

/// The ship moved so that its `hpmount` sits on the host's launch point
/// of `port`.
fn put_on_port(cx: &mut Cx<'_>, ship: ObjHandle, host: ObjHandle, port: i32) {
    let (_, point, _) = cx.world.launch_pos(host, port);
    let at = cx.world.get(ship).unwrap().engine.position;
    let mount = cx
        .world
        .hardpoint_os_world(ship, b"hpmount")
        .expect("hpmount");
    let to = Vector::new(
        point.x + (at.x - mount.x),
        point.y + (at.y - mount.y),
        point.z + (at.z - mount.z),
    );
    cx.world.beam_object(ship, to, Matrix::IDENTITY);
}

fn frame(cx: &mut Cx<'_>, system: u32, dt: f32) {
    world::step_system(cx, system, dt);
    cx.world.update_global_time(dt);
}

#[test]
fn a_dock_request_is_answered_and_queued() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("dock requests", WorldConfig::default(), &sink) else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let station_id = id(&cx, station);
    let ship = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -49000.0),
    );
    let ship_id = id(&cx, ship);
    let _ = heard();

    // `DockRequest` without a callback: the request goes to the bus once;
    // a second is refused while the first is in progress.
    assert_eq!(space_obj::dock_request(&mut cx, ship_id, station_id), 0);
    assert_eq!(
        heard(),
        [format!("DockRequest {ship_id} {station_id} {he01} true")]
    );
    assert!(
        cx.logic
            .objects
            .get(ship)
            .unwrap()
            .ship
            .as_ref()
            .unwrap()
            .request_busy
    );
    assert_eq!(space_obj::dock_request(&mut cx, ship_id, station_id), 0);
    assert!(heard().is_empty(), "busy: no second request");
    // A solar does not request (`GameObject::slot_5c`).
    assert_eq!(space_obj::dock_request(&mut cx, station_id, ship_id), -3);
    // Not found: 0.
    assert_eq!(space_obj::dock_request(&mut cx, 0x1234_5678, station_id), 0);

    // Content answers "wait" (3): the owner's client hears it, the ship
    // stays busy and nothing is queued.
    assert_eq!(space_obj::dock(&mut cx, ship_id, station_id, 0, 3), Ok(()));
    let sent = sink.take();
    assert!(
        matches!(&sent[..], [(0, ServerMessage::RequestReturned(r))]
            if r.object_id == ship_id && r.request_type == 0 && r.response == 3 && r.field_26 == 0),
        "{sent:?}"
    );
    assert!(
        cx.logic
            .objects
            .get(ship)
            .unwrap()
            .ship
            .as_ref()
            .unwrap()
            .request_busy
    );
    assert!(cx.logic.objects.get(station).unwrap().dock_queue.is_empty());

    // Then "proceed" (4) at port 0: answered, not busy any more, and the
    // station hosts the request and is kept updated once.
    assert_eq!(space_obj::dock(&mut cx, ship_id, station_id, 0, 4), Ok(()));
    let sent = sink.take();
    assert!(
        matches!(&sent[..], [(0, ServerMessage::RequestReturned(r))] if r.response == 4),
        "{sent:?}"
    );
    assert!(
        !cx.logic
            .objects
            .get(ship)
            .unwrap()
            .ship
            .as_ref()
            .unwrap()
            .request_busy
    );
    assert_eq!(
        cx.logic.objects.get(station).unwrap().dock_queue,
        [DockRequest {
            port: 0,
            ship: ship_id
        }]
    );
    assert_eq!(cx.world.always_updated(), [station]);

    // A second grant is queued too (the list keeps both), and a negative
    // port drops the ship's first request at any port.
    assert_eq!(space_obj::dock(&mut cx, ship_id, station_id, 1, 4), Ok(()));
    assert_eq!(cx.world.always_updated(), [station, station]);
    assert_eq!(space_obj::dock(&mut cx, ship_id, station_id, -1, 5), Ok(()));
    assert_eq!(
        cx.logic.objects.get(station).unwrap().dock_queue,
        [DockRequest {
            port: 1,
            ship: ship_id
        }]
    );
    assert_eq!(cx.world.always_updated(), [station]);

    // The result codes.
    assert_eq!(
        space_obj::dock(&mut cx, 0x4000_0000, station_id, 0, 4),
        Err(PubError(-4))
    );
    assert_eq!(
        space_obj::dock(&mut cx, 0x1234_5678, station_id, 0, 3),
        Err(PubError::BAD_ID),
        "a waiting answer to no ship"
    );
    assert_eq!(
        space_obj::dock(&mut cx, ship_id, 0x1234_5678, 0, 4),
        Err(PubError::NOT_FOUND)
    );

    // A cancel (`slot_60`) drops the request and tells the bus.
    assert_eq!(space_obj::dock_request(&mut cx, ship_id, station_id), 0);
    let _ = heard();
    assert_eq!(dock::cancel_request(&mut cx, ship, 0, station_id, 7), 0);
    assert_eq!(heard(), [format!("DockCancel {ship_id} {station_id} 7")]);
    let entry = cx.logic.objects.get(ship).unwrap();
    assert!(!entry.ship.as_ref().unwrap().request_busy);
    assert!(
        entry
            .invincibilities
            .iter()
            .any(|i| i.kind == 2 && !i.protect && i.max_hp_loss == 1.0)
    );
}

#[test]
fn the_queue_lands_a_ship_at_a_station() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the dock queue", WorldConfig::default(), &sink)
    else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let station_id = id(&cx, station);
    let base = cx
        .world
        .get(station)
        .unwrap()
        .as_equipped()
        .unwrap()
        .docking_point_base;
    assert_eq!(base, create_id(b"he01_11_Base"), "dock_with");
    let near = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -49000.0),
    );
    let far = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -40000.0),
    );
    let gone = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -42000.0),
    );
    let (near_id, far_id, gone_id) = (id(&cx, near), id(&cx, far), id(&cx, gone));
    put_on_port(&mut cx, near, station, 0);
    for s in [near_id, far_id, gone_id] {
        space_obj::dock(&mut cx, s, station_id, 0, 4).unwrap();
    }
    world::destroy_object(&mut cx, gone, 0, 0);
    let _ = heard();

    // One frame: the station updates without a player near (it is kept
    // updated), the ship whose `hpmount` is in the sphere joins the landing
    // queue (a station: landing, the target's base set so no target id),
    // lands after the deletes and, an NPC's, is destroyed; the ship that is
    // gone leaves the queue; the far one waits.
    frame(&mut cx, he01, 0.05);
    assert_eq!(
        heard(),
        [
            format!("Msg0x27 {near_id} {base} 0 true"),
            "ObjectDestroyed".to_owned()
        ]
    );
    assert!(cx.world.get(near).is_none_or(|o| o.dead));
    assert_eq!(
        cx.logic.objects.get(station).unwrap().dock_queue,
        [DockRequest {
            port: 0,
            ship: far_id
        }]
    );
    assert_eq!(cx.world.always_updated(), [station]);
    assert!(
        cx.logic
            .server_world
            .system(he01)
            .unwrap()
            .dock_queue
            .is_empty()
    );

    // Once the far ship is gone too, the queue empties and the station
    // stops being kept updated.
    world::destroy_object(&mut cx, far, 0, 0);
    let _ = heard();
    frame(&mut cx, he01, 0.05);
    assert!(cx.logic.objects.get(station).unwrap().dock_queue.is_empty());
    assert!(cx.world.always_updated().is_empty());
    assert!(heard().is_empty());
}

#[test]
fn a_gate_completes_the_dock_and_starts_the_death_sequence() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("complete_dock", WorldConfig::default(), &sink) else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    let gate = solar(&cx, b"he01_to_he02");
    let gate_id = id(&cx, gate);
    assert_eq!(cx.world.get(gate).unwrap().simple.type_bits, 0x40);
    let ship = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(-83176.0, 0.0, 43000.0),
    );
    let ship_id = id(&cx, ship);
    put_on_port(&mut cx, ship, gate, 0);
    space_obj::dock(&mut cx, ship_id, gate_id, 0, 4).unwrap();
    let _ = heard();

    // A gate completes the dock: `DockComplete`, the gate as the death
    // sequence's object, 12 s on a multiplayer server, no collisions.
    frame(&mut cx, he01, 0.05);
    assert_eq!(
        heard(),
        [format!("DockComplete {ship_id} {gate_id} {he01}")]
    );
    let state = cx.logic.objects.get(ship).unwrap().ship.clone().unwrap();
    assert_eq!(state.death_sequence_obj_id, gate_id);
    assert_eq!(state.death_sequence_timer, 12.0);
    assert!(cx.logic.objects.get(gate).unwrap().dock_queue.is_empty());
    // A second `complete_dock` during the sequence does nothing.
    dock::complete_dock(&mut cx, he01, ship, gate);
    assert!(heard().is_empty());

    // The sequence counts down in the ship's update and then the ship is
    // announced after the frame (`Server_PhysicsUpdate`'s list).
    for _ in 0..12 {
        frame(&mut cx, he01, 1.0);
    }
    assert!(cx.logic.server_world.dying_ships.is_empty());
    frame(&mut cx, he01, 1.0);
    assert_eq!(cx.logic.server_world.dying_ships, [ship]);
    let state = cx.logic.objects.get(ship).unwrap().ship.clone().unwrap();
    assert_eq!(state.death_sequence_obj_id, 0);
    assert_eq!(state.death_sequence_timer, 0.0);

    // A player's ship through a jump hole: the invincibility of type 2.
    let hole = solar(&cx, b"he01_to_he04_hole");
    assert_eq!(cx.world.get(hole).unwrap().simple.type_bits, 0x800);
    let mut spawn = game_sim::object::world::ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(15425.0, 0.0, 82000.0),
        Matrix::IDENTITY,
    );
    spawn.owner_player = 1;
    let player_ship = cx.world.spawn_ship(he01, &spawn).unwrap();
    let player_id = id(&cx, player_ship);
    let _ = heard();
    dock::complete_dock(&mut cx, he01, player_ship, hole);
    assert_eq!(
        heard(),
        [format!("DockComplete {player_id} {} {he01}", id(&cx, hole))]
    );
    let entry = cx.logic.objects.get(player_ship).unwrap();
    assert_eq!(
        entry.ship.as_ref().unwrap().death_sequence_obj_id,
        id(&cx, hole)
    );
    assert!(entry.invincible && entry.invincible_flag);
    assert!(
        entry
            .invincibilities
            .iter()
            .any(|i| i.kind == 2 && i.protect && i.from_players && i.max_hp_loss == 0.0)
    );
}

#[test]
fn single_player_complete_dock_waits_for_the_jump_out() {
    let sink = Shared::default();
    let config = WorldConfig {
        single_player: true,
        step_mode: neuron_core::physics::StepMode::SinglePlayer,
        server_physics: false,
        ..WorldConfig::default()
    };
    let Some((mut world, mut logic)) = setup("single-player docking", config, &sink) else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    // The client side built he01's solars in the shared world.
    cx.world.load_system(he01);
    cx.world.load_system_solars(he01);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let ship = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -49000.0),
    );
    if let Some(s) = cx.world.get_mut(ship).and_then(|o| o.as_ship_mut()) {
        s.jumped_out = true;
    }
    let _ = heard();

    // In single player an NPC's dock waits for the client's jump-out
    // (cleared here), with its collisions kept.
    dock::complete_dock(&mut cx, he01, ship, station);
    assert_eq!(heard().len(), 1);
    assert!(!cx.world.get(ship).unwrap().as_ship().unwrap().jumped_out);
    let state = cx.logic.objects.get(ship).unwrap().ship.clone().unwrap();
    assert_eq!(state.death_sequence_obj_id, id(&cx, station));
    assert_eq!(state.death_sequence_timer, 0.0);
    frame(&mut cx, he01, 0.05);
    assert!(
        cx.logic.server_world.dying_ships.is_empty(),
        "not jumped out"
    );
    if let Some(s) = cx.world.get_mut(ship).and_then(|o| o.as_ship_mut()) {
        s.jumped_out = true;
    }
    frame(&mut cx, he01, 0.05);
    assert_eq!(cx.logic.server_world.dying_ships, [ship]);
}

#[test]
fn instant_dock_lands_at_a_station_and_completes_at_a_gate() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("InstantDock", WorldConfig::default(), &sink) else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let gate = solar(&cx, b"he01_to_he02");
    let (station_id, gate_id) = (id(&cx, station), id(&cx, gate));
    let a = fighter(&mut cx, controller, he01, Vector::new(0.0, 0.0, -45000.0));
    let b = fighter(&mut cx, controller, he01, Vector::new(0.0, 0.0, -46000.0));
    let (a_id, b_id) = (id(&cx, a), id(&cx, b));
    let _ = heard();

    // Always -2; a station queues the landing at once, a gate completes.
    assert_eq!(
        space_obj::instant_dock(&mut cx, a_id, station_id, 0),
        Err(PubError::BAD_ID)
    );
    let queue = &cx.logic.server_world.system(he01).unwrap().dock_queue;
    assert_eq!(queue.len(), 1);
    assert_eq!(
        (queue[0].object, queue[0].target, queue[0].land),
        (Some(a), Some(station), true)
    );
    assert_eq!(
        space_obj::instant_dock(&mut cx, b_id, gate_id, 0),
        Err(PubError::BAD_ID)
    );
    assert_eq!(heard(), [format!("DockComplete {b_id} {gate_id} {he01}")]);
    assert_eq!(
        space_obj::instant_dock(&mut cx, 0, gate_id, 0),
        Err(PubError(-4))
    );
}

#[test]
fn the_ai_request_reaches_the_server_after_the_behaviour_tick() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the AI's request", WorldConfig::default(), &sink)
    else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let station_id = id(&cx, station);
    // A player in the system, so that the multiplayer frame steps the
    // physics and the ship flies.
    cx.logic.players.get_mut(1).unwrap().client_id = 1;
    cx.logic.players.get_mut(1).unwrap().system_id = he01;
    world::add_player(&mut cx, he01, 1);
    let ship = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(6173.0, 0.0, -49300.0),
    );
    let ship_id = id(&cx, ship);
    let graph = cx
        .world
        .data()
        .state_graphs
        .find(b"FIGHTER", game_sim::ai::state_graph::GraphType::Leader);
    assert_eq!(cx.world.submit_state(ship, graph, None), 0);
    let op = DirectiveOp::Dock(DockOp {
        dock_object: station_id,
        ..DockOp::default()
    });
    cx.world.submit_directive(ship, &op);
    let _ = heard();
    let _ = sink.take();

    // The dock behaviour flies in and asks for a port; the server hears
    // the request with the behaviour as its callback.
    let mut asked = false;
    for _ in 0..2000 {
        frame(&mut cx, he01, 0.05);
        if heard()
            .iter()
            .any(|l| l == &format!("DockRequest {ship_id} {station_id} {he01} false"))
        {
            asked = true;
            break;
        }
    }
    assert!(asked, "the dock behaviour asked for a port");
    let state = cx.logic.objects.get(ship).unwrap().ship.clone().unwrap();
    assert!(state.request_busy && state.request_callback);
    assert!(sink.take().is_empty());

    // Content's refusal (5, no port) reaches the behaviour, not a client,
    // and ends the request.
    assert_eq!(space_obj::dock(&mut cx, ship_id, station_id, -1, 5), Ok(()));
    assert!(
        !sink
            .take()
            .iter()
            .any(|(_, m)| matches!(m, ServerMessage::RequestReturned(_)))
    );
    let state = cx.logic.objects.get(ship).unwrap().ship.clone().unwrap();
    assert!(!state.request_busy && !state.request_callback);
    let manager = cx.world.behavior_manager(ship).unwrap();
    assert!(!manager.state.request_busy && manager.state.pending_request.is_none());
}

#[test]
fn a_player_launches_from_a_base() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a launch", WorldConfig::default(), &sink) else {
        return;
    };
    let he01 = create_id(b"he01");
    let arch = create_id(b"he_fighter");
    let base = create_id(b"he01_01_Base");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    world::load(&mut cx, he01);
    let ring = solar(&cx, b"Dock_Ring_he01_01");
    let ring_id = id(&cx, ring);
    assert_eq!(cx.world.get(ring).unwrap().simple.type_bits & 0x20, 0x20);
    let group = cx.world.data().archetypes.ships[&arch]
        .eq_obj
        .collision_groups
        .first()
        .map(|g| g.id)
        .expect("a collision group");
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01;
        slot.ship_archetype = arch;
        slot.relative_health = 0.5;
        slot.exited_base_id = base;
        slot.rep_vibe = -1;
        slot.collision_groups = vec![CollisionGroupDesc {
            id: group,
            health: 0.25,
        }];
    }

    // The client asks for its ship's id first; a second ask answers the
    // same id.
    dock::request_create_ship(&mut cx, 1);
    let ship_id = cx.logic.players.get(1).unwrap().created_ship_id;
    assert_ne!(ship_id, 0);
    dock::request_create_ship(&mut cx, 1);
    let sent = sink.take();
    assert_eq!(sent.len(), 2);
    assert!(sent.iter().all(|(c, m)| *c == 1
        && matches!(m, ServerMessage::RequestCreateShipResp(r) if r.field_00 == 1 && r.field_04 == ship_id)));
    let _ = heard();

    // A mismatched ship id launches nothing.
    dock::player_launch(&mut cx, ship_id + 1, 1);
    assert!(heard().is_empty());

    // The expected point: the ring's launch point 1 (a docking ring), the
    // ship's origin behind it by the archetype's `HpMount`.
    let (_, point, orient) = cx.world.launch_pos(ring, 1);
    let mount = cx
        .world
        .ship_archetype_hardpoint(arch, b"HpMount")
        .map_or(Vector::ZERO, |t| t.translation);
    let q = neuron_core::math::matrix_to_quaternion(&orient);
    let offset = transform_matrix(&quaternion_to_matrix(&q), mount);
    let sub = |a: f32, b: f32| (f64::from(a) - f64::from(b)) as f32;
    let expected = Vector::new(
        sub(point.x, offset.x),
        sub(point.y, offset.y),
        sub(point.z, offset.z),
    );

    dock::player_launch(&mut cx, ship_id, 1);
    assert_eq!(
        heard(),
        [
            format!("PlayerLaunch 1 {base}"),
            format!("SpaceEnter {he01} 1 {ring_id} 1"),
        ]
    );
    assert!(cx.logic.server_world.system(he01).unwrap().has_player(1));
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!(slot.ship_object_id, ship_id);
    assert_eq!(slot.exited_base_id, 0, "the base left is forgotten");
    let sent = sink.take();
    let launch = sent
        .iter()
        .find_map(|(c, m)| match m {
            ServerMessage::Launch(l) if *c == 1 => Some(l.clone()),
            _ => None,
        })
        .expect("the launch packet");
    assert_eq!(
        (launch.field_00, launch.field_04, launch.field_08),
        (ship_id, ring_id, 1)
    );
    assert_eq!(launch.position, [expected.x, expected.y, expected.z]);
    assert_eq!(launch.orientation, [q.w, q.x, q.y, q.z]);
    assert!(
        sent.iter()
            .filter(|(_, m)| matches!(m, ServerMessage::MiscObjUpdate(u) if u.field_28 == ship_id))
            .count()
            == 2,
        "the ship's group and affiliation on a multiplayer server"
    );

    // The ship: the player's, at the point, half its hit points, only the
    // slot's collision group at a quarter, and invincible for the launch.
    let h = cx.world.find(ship_id).expect("the launched ship");
    let o = cx.world.get(h).unwrap();
    assert_eq!(o.simple.owner_player, 1);
    assert_eq!(o.engine.position, expected);
    let arch_hp = cx.world.data().archetypes.ships[&arch].eq_obj.root.hit_pts;
    assert_eq!(
        o.simple.hit_pts,
        neuron_core::num::trunc_to_i32(0.5 * f64::from(arch_hp)) as f32
    );
    let groups = &o.as_equipped().unwrap().arch_groups;
    assert_eq!(groups.len(), 1);
    assert_eq!(groups[0].id, group);
    assert_eq!(
        groups[0].hit_pts,
        (f64::from(groups[0].max_hit_pts) * 0.25) as f32
    );
    let entry = cx.logic.objects.get(h).unwrap();
    assert!(entry.ship.as_ref().unwrap().launch_invincibility);
    assert!(entry.invincible);

    // `LaunchComplete` lifts it.
    assert_eq!(ship::launch_complete(&mut cx, h, ring_id), 0);
    assert!(!cx.logic.objects.get(h).unwrap().invincible);
}

#[test]
fn a_launch_from_a_station_asks_for_a_free_terminal() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a station launch", WorldConfig::default(), &sink)
    else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    world::load(&mut cx, he01);
    let station = solar(&cx, b"he01_11");
    let station_id = id(&cx, station);
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01;
        slot.ship_archetype = create_id(b"he_fighter");
        slot.relative_health = 1.0;
        slot.exited_base_id = create_id(b"he01_11_Base");
    }
    let _ = heard();
    // No handler reports a terminal: point 0.
    let launch = dock::launch_position(&mut cx, 1);
    assert_eq!(heard(), [format!("FreeTerminalRequest 1 {station_id}")]);
    assert_eq!((launch.object, launch.kind), (station_id, 0));
    let (_, point, _) = cx.world.launch_pos(station, 0);
    assert_eq!(launch.position, point);

    // Without a base left or a starting ring: the slot's initial position.
    let initial = dock::launch_position(&mut cx, 1);
    assert_eq!((initial.object, initial.kind), (0, -1));
    assert_eq!(initial.position, Vector::ZERO);
}
