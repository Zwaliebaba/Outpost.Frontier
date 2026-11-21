//! Moving a ship between systems over the original data (`outdir/`, the
//! rebranded he01 and he02 joined by the gates `he01_to_he02` and
//! `he02_to_he01`): an NPC's jump (the gate's death sequence, Content's
//! `pub::SpaceObj::Relocate` into transit and `JumpIn` at the destination
//! gate with the world's `JumpInAction`), a player's gate jump
//! (`IServerImpl::SystemSwitchOutComplete` with the players' system switch
//! and `JumpInComplete`), `JumpIn` and `Relocate` and their result codes,
//! and single player's unload of the system left. Expected values follow
//! the C++ (`Server/IServerImpl.cpp`, `server_classes.cpp`,
//! `server_gameobject.cpp`, `server_pub.cpp`,
//! `Common/common_launchaction.cpp`).

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::PubError;
use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::{transfer, world};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ControllerRef, ShipSpawn, WorldConfig};
use game_sim::object::{KindData, ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Transform, Vector, matrix_to_quaternion, transform_xform};
use neuron_core::physics::StepMode;
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

fn single_player() -> WorldConfig {
    WorldConfig {
        single_player: true,
        step_mode: StepMode::SinglePlayer,
        server_physics: false,
        ..WorldConfig::default()
    }
}

fn setup(what: &str, config: WorldConfig, sink: &Shared) -> Option<(World, Logic)> {
    let Some(data_config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &data_config, None);
    let multiplayer = !config.single_player;
    let world = World::new(Arc::new(data), config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, multiplayer);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::DockComplete { ship, dock, system } => {
                format!("DockComplete {ship} {dock} {system}")
            }
            ControllerMessage::Msg0x25 {
                ship,
                death_sequence,
                system,
            } => format!("Msg0x25 {ship} {death_sequence} {system}"),
            ControllerMessage::JumpInComplete { arg, ship } => {
                format!("JumpInComplete {arg} {ship}")
            }
            ControllerMessage::SystemExit {
                system,
                destination,
                player,
            } => format!("SystemExit {system} {destination} {player}"),
            ControllerMessage::SystemEnter {
                system,
                from,
                player,
                arrival,
                position,
            } => format!(
                "SystemEnter {system} {from} {player} {arrival} {} {} {}",
                position.x, position.y, position.z
            ),
            ControllerMessage::SystemLoaded { system } => format!("SystemLoaded {system}"),
            ControllerMessage::SystemUnloaded { system } => format!("SystemUnloaded {system}"),
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

/// A Content controller, which owns the NPC ships.
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

fn pose(cx: &Cx<'_>, h: ObjHandle) -> (Vector, Matrix) {
    let o = cx.world.get(h).unwrap();
    (o.engine.position, o.engine.orientation)
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

/// A player in a system with a ship of its own at `at`.
fn player(cx: &mut Cx<'_>, session: u32, system: u32, at: Vector) -> ObjHandle {
    {
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = system;
    }
    world::add_player(cx, system, session);
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    spawn.owner_player = session;
    let h = cx.world.spawn_ship(system, &spawn).expect("the ship");
    let ship_id = id(cx, h);
    cx.logic.players.get_mut(session).unwrap().ship_object_id = ship_id;
    let _ = cx.logic.objects.entry(cx.world, h);
    h
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

/// `Server_PhysicsUpdate` and the global time.
fn tick(cx: &mut Cx<'_>, dt: f32) {
    world::physics_update(cx, dt);
    cx.world.update_global_time(dt);
}

/// The arrival point of `Solar::ChooseSystemArrivalLocation` for a gate:
/// the local `(cos a · 500, sin a · 500, -350)` through the gate's
/// transform, `a` from some `rand()` draw. Checks the point lies on that
/// ring (to the float rounding) and that one of the 32768 draws gives it
/// exactly.
fn on_arrival_ring(gate_position: Vector, gate_rotation: Matrix, at: Vector) -> bool {
    let d = [
        f64::from(at.x) - f64::from(gate_position.x),
        f64::from(at.y) - f64::from(gate_position.y),
        f64::from(at.z) - f64::from(gate_position.z),
    ];
    let m = gate_rotation.m;
    let local = |c: usize| m[0][c] as f64 * d[0] + m[1][c] as f64 * d[1] + m[2][c] as f64 * d[2];
    let (lx, ly, lz) = (local(0), local(1), local(2));
    let ring = (lx * lx + ly * ly).sqrt();
    if (lz + 350.0).abs() > 0.05 || (ring - 500.0).abs() > 0.05 {
        return false;
    }
    let xform = Transform::new(gate_rotation, gate_position);
    (0..0x8000).any(|draw: i32| {
        let angle = f64::from(draw as f32 * (1.0 / 32768.0)) * f64::from(std::f32::consts::TAU);
        let local = Vector::new(
            (angle.cos() * 500.0) as f32,
            (angle.sin() * 500.0) as f32,
            -350.0,
        );
        transform_xform(&xform, local) == at
    })
}

fn has_controller(cx: &Cx<'_>, h: ObjHandle, which: ControllerRef) -> bool {
    cx.world
        .get(h)
        .unwrap()
        .physical
        .controllers
        .iter()
        .any(|(w, _, _)| *w == which)
}

fn collisions(cx: &Cx<'_>, h: ObjHandle) -> bool {
    let (system, body) = cx.world.body(h).expect("a body");
    cx.world.physics(system).unwrap().collision_state(body)
}

fn body_mass(cx: &Cx<'_>, h: ObjHandle) -> f32 {
    let (system, body) = cx.world.body(h).expect("a body");
    cx.world.physics(system).unwrap().mass(body).unwrap()
}

fn tracks(cx: &Cx<'_>, system: u32, session: u32, h: ObjHandle) -> bool {
    cx.logic
        .server_world
        .system(system)
        .unwrap()
        .observers
        .iter()
        .find(|o| o.player == session)
        .is_some_and(|o| o.find(h).is_some())
}

#[test]
fn an_npc_jumps_from_he01_to_he02() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("an NPC's jump", WorldConfig::default(), &sink) else {
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    // A player near each gate: the multiplayer frames step the physics and
    // the observers see the NPC.
    player(&mut cx, 1, he01, Vector::new(-83176.0, 0.0, 43500.0));
    player(&mut cx, 2, he02, Vector::new(60000.0, 0.0, -43000.0));
    let gate = solar(&cx, b"he01_to_he02");
    let gate_id = id(&cx, gate);

    // The destination is the gate's `goto`: he02 and its gate he02_to_he01.
    let (dest_system, dest_gate) = match &cx.world.get(gate).unwrap().data {
        KindData::Solar(s) => (s.dest_system, s.dest_gate),
        _ => panic!("a solar"),
    };
    assert_eq!((dest_system, dest_gate), (he02, create_id(b"he02_to_he01")));
    let arrival = world_dest_gate(&cx, he02, dest_gate);

    let npc = fighter(
        &mut cx,
        controller,
        he01,
        Vector::new(-83176.0, 0.0, 43000.0),
    );
    let npc_id = id(&cx, npc);
    let mass = body_mass(&cx, npc);
    // Player 1's client gets the NPC.
    tick(&mut cx, 0.01);
    assert!(tracks(&cx, he01, 1, npc));
    put_on_port(&mut cx, npc, gate, 0);
    space_obj::dock(&mut cx, npc_id, gate_id, 0, 4).unwrap();
    let _ = (heard(), sink.take());

    // The gate takes the ship in: `DockComplete`, the players' switch-out
    // (`OnObjectLeftSystem`), 12 s of death sequence without collisions.
    tick(&mut cx, 0.05);
    assert_eq!(heard(), [format!("DockComplete {npc_id} {gate_id} {he01}")]);
    let sent = sink.take();
    assert!(
        sent.iter().any(|(c, m)| *c == 1
            && matches!(m, ServerMessage::SystemSwitchOut(s) if s.field_00 == npc_id && s.field_04 == gate_id)),
        "{sent:?}"
    );
    assert!(!tracks(&cx, he01, 1, npc), "the record went");
    assert!(!collisions(&cx, npc));

    // The sequence ends: the dying ship is announced after the frame.
    let mut announced = false;
    for _ in 0..14 {
        tick(&mut cx, 1.0);
        announced |= heard().contains(&format!("Msg0x25 {npc_id} 0 {he01}"));
    }
    assert!(announced, "the dying ship's Msg0x25");

    // Content moves it into transit (`Relocate` to system 0): out of
    // he01's lists, no body, its physics parameters kept, found by id.
    assert_eq!(
        space_obj::relocate(&mut cx, npc_id, 0, Vector::ZERO, Matrix::IDENTITY),
        Ok(())
    );
    assert!(cx.world.in_transit(npc));
    assert!(!cx.world.objects_in(he01).contains(&npc));
    assert!(cx.world.body(npc).is_none());
    assert_eq!(cx.world.get(npc).unwrap().simple.system, None);
    let parms = cx.world.transit_parms(npc).expect("the saved parameters");
    assert_eq!(parms.mass, mass);
    assert!(
        parms.enable_collisions,
        "the end of the sequence gave them back"
    );
    assert_eq!(cx.world.find(npc_id), Some(npc));
    assert!(
        sink.take()
            .iter()
            .all(|(_, m)| !matches!(m, ServerMessage::DestroyObject(_))),
        "no observer had it any more"
    );

    // ... and in at the destination gate (`JumpIn`): the ship enters he02
    // at the gate's arrival point with the gate's orientation, its body
    // rebuilt from the parameters and its controllers on, and jumps in
    // (`JumpInAction`: collisions off, beamed to the point).
    let arrival_id = id(&cx, arrival);
    let (gate_position, gate_rotation) = pose(&cx, arrival);
    let _ = heard();
    assert_eq!(space_obj::jump_in(&mut cx, npc_id, arrival_id), Ok(()));
    assert!(!cx.world.in_transit(npc));
    assert!(cx.world.objects_in(he02).contains(&npc));
    let o = cx.world.get(npc).unwrap();
    assert_eq!((o.physical.system, o.simple.system), (he02, Some(he02)));
    let (at, orientation) = pose(&cx, npc);
    assert_eq!(orientation, gate_rotation);
    assert!(on_arrival_ring(gate_position, gate_rotation, at), "{at:?}");
    assert_eq!(body_mass(&cx, npc), mass);
    for which in [
        ControllerRef::Steering,
        ControllerRef::Nudge,
        ControllerRef::Strafe,
        ControllerRef::Ship,
        ControllerRef::JumpIn,
    ] {
        assert!(has_controller(&cx, npc, which), "{which:?}");
    }
    assert!(cx.world.is_jumping(npc));
    let state = cx.world.jump_in_state(npc).unwrap().clone();
    assert_eq!((state.speed, state.approach_budget), (0.5, 1000.0));
    assert_eq!((state.position, state.orientation), (at, gate_rotation));
    assert!(!collisions(&cx, npc));
    let throttle = cx
        .world
        .get(npc)
        .unwrap()
        .as_ship()
        .unwrap()
        .flight
        .throttle;
    assert_eq!(throttle, 0.0);
    // Player 2 tracks it from `add_ship`, but its client has not been told
    // of it yet, so the switch-in is not visible to it
    // (`SystemPlayerView_IsObjectVisible` of an uncreated record); the next
    // frame's pass creates it there.
    assert!(tracks(&cx, he02, 2, npc));
    let sent = sink.take();
    assert!(
        sent.iter()
            .all(|(_, m)| !matches!(m, ServerMessage::SystemSwitchIn(_))),
        "{sent:?}"
    );
    assert!(heard().is_empty());

    // The approach takes 0.5 s of PSIs; at its end the ship is at the
    // point, the arrival is reported (`JumpInComplete` with he02) and the
    // next update removes the action, which turns the collisions back on.
    tick(&mut cx, 0.1);
    let sent = sink.take();
    assert!(
        sent.iter().any(|(c, m)| *c == 2
            && matches!(m, ServerMessage::CreateShip(s) if s.object.obj_id == npc_id)),
        "{sent:?}"
    );
    let (eased, _) = pose(&cx, npc);
    assert_ne!(eased, at, "on its way along the gate's axis");
    for _ in 0..3 {
        tick(&mut cx, 0.1);
    }
    assert!(heard().is_empty(), "still jumping in");
    assert!(!cx.world.jump_in_state(npc).unwrap().complete);
    // 0.5 s of PSIs: thirty of 1/60 s, whose float sum reaches 0.5 in the
    // fifth frame.
    let mut frames = 4;
    let mut lines = Vec::new();
    while lines.is_empty() && frames < 8 {
        tick(&mut cx, 0.1);
        frames += 1;
        lines = heard();
    }
    assert_eq!(lines, [format!("JumpInComplete {he02} {npc_id}")]);
    assert_eq!(frames, 5);
    assert!(cx.world.jump_in_state(npc).unwrap().complete);
    assert_eq!(pose(&cx, npc), (at, gate_rotation), "beamed to the point");
    tick(&mut cx, 0.1);
    assert!(!cx.world.is_jumping(npc));
    assert!(!has_controller(&cx, npc, ControllerRef::JumpIn));
    assert!(collisions(&cx, npc));
    assert!(heard().is_empty(), "reported once");
    // The jump's invincibility of type 2 ends (`Ship::slot_74`).
    let entry = cx.logic.objects.get(npc).unwrap();
    assert!(
        entry
            .invincibilities
            .iter()
            .any(|i| i.kind == 2 && !i.protect && !i.from_players && i.max_hp_loss == 1.0)
    );
}

/// The destination gate of a jump: the system's solar with the `goto`
/// gate's nickname.
fn world_dest_gate(cx: &Cx<'_>, system: u32, nickname: u32) -> ObjHandle {
    game_logic::dock::find_solar(cx, system, nickname).expect("the arrival gate")
}

#[test]
fn a_player_jumps_from_he01_to_he02() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a player's jump", WorldConfig::default(), &sink)
    else {
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    let ship = player(&mut cx, 1, he01, Vector::new(-83176.0, 0.0, 43500.0));
    player(&mut cx, 2, he01, Vector::new(-83176.0, 0.0, 42500.0));
    player(&mut cx, 3, he02, Vector::new(60500.0, 0.0, -43000.0));
    let ship_id = id(&cx, ship);
    let gate = solar(&cx, b"he01_to_he02");
    let gate_id = id(&cx, gate);
    let arrival = world_dest_gate(&cx, he02, create_id(b"he02_to_he01"));
    let arrival_id = id(&cx, arrival);
    let (gate_position, gate_rotation) = pose(&cx, arrival);
    tick(&mut cx, 0.01);
    assert!(tracks(&cx, he01, 2, ship), "player 2 sees player 1's ship");
    let mass = body_mass(&cx, ship);

    // The gate completes the dock: the ship is invincible and the clients
    // that had it hear it switch out.
    game_logic::dock::complete_dock(&mut cx, he01, ship, gate);
    let entry = cx.logic.objects.get(ship).unwrap();
    assert!(entry.invincible);
    assert_eq!(entry.ship.as_ref().unwrap().death_sequence_obj_id, gate_id);
    let sent = sink.take();
    for client in [1, 2] {
        assert!(
            sent.iter().any(|(c, m)| *c == client
                && matches!(m, ServerMessage::SystemSwitchOut(s) if s.field_00 == ship_id && s.field_04 == gate_id)),
            "{client}: {sent:?}"
        );
    }
    let _ = heard();

    // Invalid reports change nothing: another session's ship, a session
    // in a base, a session without a client.
    transfer::system_switch_out_complete(&mut cx, ship_id, 2);
    cx.logic.players.get_mut(1).unwrap().current_base = create_id(b"he01_01_base");
    transfer::system_switch_out_complete(&mut cx, ship_id, 1);
    cx.logic.players.get_mut(1).unwrap().current_base = 0;
    transfer::system_switch_out_complete(&mut cx, ship_id, 4);
    assert!(heard().is_empty() && sink.take().is_empty());

    // The client's jump-out is done: `Msg0x25`, the player out of he01
    // (`SystemExit`) and into he02, the ship at the arrival point,
    // `SystemEnter`.
    transfer::system_switch_out_complete(&mut cx, ship_id, 1);
    let (at, orientation) = pose(&cx, ship);
    assert_eq!(
        heard(),
        [
            format!("Msg0x25 {ship_id} {gate_id} {he01}"),
            format!("SystemExit {he01} {he02} 1"),
            format!(
                "SystemEnter {he02} {he01} 1 {arrival_id} {} {} {}",
                at.x, at.y, at.z
            ),
        ]
    );
    assert_eq!(orientation, gate_rotation);
    assert!(on_arrival_ring(gate_position, gate_rotation, at), "{at:?}");
    let sys = cx.logic.server_world.system(he01).unwrap();
    assert!(!sys.has_player(1) && sys.has_player(2));
    assert!(!tracks(&cx, he01, 2, ship));
    assert!(cx.logic.server_world.system(he02).unwrap().has_player(1));
    assert_eq!(cx.logic.players.get(1).unwrap().system_id, he02);
    assert!(!cx.world.objects_in(he01).contains(&ship));
    assert!(cx.world.objects_in(he02).contains(&ship));
    let o = cx.world.get(ship).unwrap();
    assert!(!o.dead, "the old system's removal of the player spares it");
    assert_eq!((o.physical.system, o.simple.system), (he02, Some(he02)));
    assert_eq!(body_mass(&cx, ship), mass);
    assert!(has_controller(&cx, ship, ControllerRef::Steering));
    // A player's ship: no jump-in action (its client plays the arrival),
    // under remote correction again, the death sequence over.
    assert!(!cx.world.is_jumping(ship));
    assert!(cx.world.rc_active(ship));
    let entry = cx.logic.objects.get(ship).unwrap();
    assert_eq!(entry.ship.as_ref().unwrap().death_sequence_obj_id, 0);
    assert!(entry.invincible, "until the jump-in completes");

    // The packets: every session hears player 1's new system first; player
    // 1 then gets he02's objects and the switch-in. Player 3 tracks the
    // ship from `add_ship` but its client has no create yet, so the
    // switch-in is not visible to it; the next frame creates it there.
    // Player 2 hears no destroy: its record went with the switch-out.
    let sent = sink.take();
    let system_ids: Vec<u32> = sent
        .iter()
        .take_while(|(_, m)| matches!(m, ServerMessage::MiscObjUpdate(u) if u.flags == 0x84))
        .map(|(c, _)| *c)
        .collect();
    assert_eq!(system_ids, [1, 2, 3]);
    let q = matrix_to_quaternion(&gate_rotation);
    let switched_in: Vec<u32> = sent
        .iter()
        .filter(|(_, m)| {
            matches!(m, ServerMessage::SystemSwitchIn(s) if s.field_00 == ship_id
            && s.field_04 == 0x40
            && s.position == [at.x, at.y, at.z]
            && s.orientation == [q.w, q.x, q.y, q.z])
        })
        .map(|(c, _)| *c)
        .collect();
    assert_eq!(switched_in, [1]);
    assert!(
        sent.iter()
            .all(|(c, m)| *c != 2 || !matches!(m, ServerMessage::DestroyObject(_)))
    );
    assert!(tracks(&cx, he02, 3, ship), "player 3 tracks the new ship");
    tick(&mut cx, 0.01);
    let sent = sink.take();
    assert!(
        sent.iter().any(|(c, m)| *c == 3
            && matches!(m, ServerMessage::CreateShip(s) if s.object.obj_id == ship_id)),
        "{sent:?}"
    );
    let _ = heard();

    // The client's `JumpInComplete` ends the jump's invincibility.
    transfer::jump_in_complete(&mut cx, he02, ship_id);
    assert_eq!(heard(), [format!("JumpInComplete {he02} {ship_id}")]);
    assert!(!cx.logic.objects.get(ship).unwrap().invincible);

    // A second report finds the ship's death sequence over and does
    // nothing.
    transfer::system_switch_out_complete(&mut cx, ship_id, 1);
    assert!(heard().is_empty());
}

#[test]
fn jump_in_and_relocate() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("JumpIn and Relocate", WorldConfig::default(), &sink)
    else {
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01);
    world::load(&mut cx, he02);
    let gate = solar(&cx, b"he02_to_he01");
    let gate_id = id(&cx, gate);
    let (gate_position, gate_rotation) = pose(&cx, gate);
    let npc = fighter(&mut cx, controller, he02, Vector::new(0.0, 0.0, 0.0));
    let other = fighter(&mut cx, controller, he02, Vector::new(500.0, 0.0, 0.0));
    let (npc_id, other_id) = (id(&cx, npc), id(&cx, other));

    // The result codes; nothing moves.
    assert_eq!(space_obj::jump_in(&mut cx, 0, gate_id), Err(PubError(-4)));
    assert_eq!(
        space_obj::jump_in(&mut cx, 0x1234_5678, gate_id),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        space_obj::jump_in(&mut cx, npc_id, 0x1234_5678),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        space_obj::jump_in(&mut cx, npc_id, other_id),
        Err(PubError::NOT_FOUND),
        "not a solar (D684)"
    );
    assert_eq!(pose(&cx, npc).0, Vector::ZERO);

    // `JumpIn` within the gate's own system: no transfer, the arrival
    // point and the jump-in.
    assert_eq!(space_obj::jump_in(&mut cx, npc_id, gate_id), Ok(()));
    let (at, orientation) = pose(&cx, npc);
    assert!(on_arrival_ring(gate_position, gate_rotation, at), "{at:?}");
    assert_eq!(orientation, gate_rotation);
    assert!(cx.world.is_jumping(npc));
    assert!(cx.world.objects_in(he02).contains(&npc));

    // `Relocate` within the system beams the object.
    let to = Vector::new(1000.0, 2000.0, 3000.0);
    assert_eq!(
        space_obj::relocate(&mut cx, other_id, he02, to, Matrix::IDENTITY),
        Ok(())
    );
    assert_eq!(pose(&cx, other), (to, Matrix::IDENTITY));
    assert!(!cx.world.in_transit(other));

    // Into transit: system 0, or an id with no system; out of transit again
    // into he01, where a ship's control exclusion is cleared.
    assert_eq!(
        space_obj::relocate(&mut cx, other_id, 0, to, Matrix::IDENTITY),
        Ok(())
    );
    assert!(cx.world.in_transit(other));
    assert_eq!(
        space_obj::relocate(&mut cx, other_id, 0x1234_5678, to, Matrix::IDENTITY),
        Ok(()),
        "an unknown system keeps it in transit"
    );
    assert!(cx.world.in_transit(other));
    cx.world
        .get_mut(other)
        .unwrap()
        .equipped
        .as_deref_mut()
        .unwrap()
        .control_exclusion = 3;
    let there = Vector::new(-20000.0, 0.0, 10000.0);
    assert_eq!(
        space_obj::relocate(&mut cx, other_id, he01, there, Matrix::IDENTITY),
        Ok(())
    );
    assert!(cx.world.objects_in(he01).contains(&other));
    assert_eq!(pose(&cx, other), (there, Matrix::IDENTITY));
    let o = cx.world.get(other).unwrap();
    assert_eq!(o.equipped.as_deref().unwrap().control_exclusion, 0);
    assert_eq!(o.simple.system, Some(he01));
    assert!(collisions(&cx, other));

    // From he01 through a gate in he02: the ship changes system.
    assert_eq!(space_obj::jump_in(&mut cx, other_id, gate_id), Ok(()));
    assert!(cx.world.objects_in(he02).contains(&other));
    assert!(!cx.world.objects_in(he01).contains(&other));
    assert!(on_arrival_ring(
        gate_position,
        gate_rotation,
        pose(&cx, other).0
    ));
    // From transit through the gate.
    space_obj::relocate(&mut cx, other_id, 0, there, Matrix::IDENTITY).unwrap();
    assert!(cx.world.in_transit(other));
    assert_eq!(space_obj::jump_in(&mut cx, other_id, gate_id), Ok(()));
    assert!(cx.world.objects_in(he02).contains(&other));
    assert!(cx.world.is_jumping(other));

    assert_eq!(
        space_obj::relocate(&mut cx, 0, he01, there, Matrix::IDENTITY),
        Err(PubError(-4))
    );
    assert_eq!(
        space_obj::relocate(&mut cx, 0x1234_5678, he01, there, Matrix::IDENTITY),
        Err(PubError::BAD_ID)
    );
}

#[test]
fn single_player_unloads_the_system_left() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("single player's jump", single_player(), &sink) else {
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    // The client side built he01's solars in the shared world.
    cx.world.load_system(he01);
    cx.world.load_system_solars(he01);
    let ship = player(&mut cx, 1, he01, Vector::new(-83176.0, 0.0, 43500.0));
    let ship_id = id(&cx, ship);
    let gate = solar(&cx, b"he01_to_he02");
    let gate_id = id(&cx, gate);
    let he01_objects = cx.world.objects_in(he01).len();
    assert!(he01_objects > 1);
    game_logic::dock::complete_dock(&mut cx, he01, ship, gate);
    // The client side builds he02 for the arrival.
    cx.world.load_system(he02);
    cx.world.load_system_solars(he02);
    let arrival = world_dest_gate(&cx, he02, create_id(b"he02_to_he01"));
    let arrival_id = id(&cx, arrival);
    let (gate_position, gate_rotation) = pose(&cx, arrival);
    let _ = (heard(), sink.take());

    transfer::system_switch_out_complete(&mut cx, ship_id, 1);
    let at = pose(&cx, ship).0;
    // he01 shuts down as the player leaves it (no `SystemLoaded` for he01
    // ever came: the client loaded it), he02 loads as the player arrives.
    assert_eq!(
        heard(),
        [
            format!("Msg0x25 {ship_id} {gate_id} {he01}"),
            format!("SystemExit {he01} {he02} 1"),
            format!("SystemUnloaded {he01}"),
            format!("SystemLoaded {he02}"),
            format!(
                "SystemEnter {he02} {he01} 1 {arrival_id} {} {} {}",
                at.x, at.y, at.z
            ),
        ]
    );
    assert!(!cx.logic.server_world.system(he01).unwrap().loaded);
    assert!(!cx.world.system_loaded(he01));
    assert!(cx.world.objects_in(he01).is_empty());
    assert!(cx.world.find(gate_id).is_none(), "he01's objects are gone");
    let sys = cx.logic.server_world.system(he02).unwrap();
    assert!(sys.loaded && sys.has_player(1));
    assert!(cx.world.objects_in(he02).contains(&ship));
    assert!(on_arrival_ring(gate_position, gate_rotation, at));
    // Single player: no system id to the sessions, no remote correction,
    // and the switch-in to the player.
    assert!(!cx.world.rc_active(ship));
    let sent = sink.take();
    assert!(
        sent.iter()
            .all(|(_, m)| !matches!(m, ServerMessage::MiscObjUpdate(u) if u.flags == 0x84))
    );
    assert!(
        sent.iter().any(|(c, m)| *c == 1
            && matches!(m, ServerMessage::SystemSwitchIn(s) if s.field_00 == ship_id)),
        "{sent:?}"
    );
}
