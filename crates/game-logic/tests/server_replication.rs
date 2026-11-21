//! Replication over the original data (`outdir/`, record 0007 point 7):
//! the observers' visibility rule at each range and for the owner, single
//! player's creates at once, a multiplayer frame that creates an object
//! coming into range and destroys it when it leaves or dies, the updates
//! ordered by simulation time, the link-saturation gate, the creators'
//! fan-outs and the deferred damage. The expected packets follow the C++
//! (`Server/server_observer.cpp`, `server_classes.cpp`).

use std::cell::{Cell, RefCell};
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::munitions;
use game_logic::replication::{self, Observer};
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{Fate, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, LootSpawn, ProjectileSpawn, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::to_client::ServerMessage;
use game_sim::protocol::{ToClient, quantise_orientation};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector, matrix_to_quaternion};
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

/// A client sink that keeps what it is sent and reports a link saturation
/// the test sets.
#[derive(Clone, Default)]
struct Shared {
    sent: Rc<RefCell<Vec<(u32, ToClient)>>>,
    saturation: Rc<Cell<f64>>,
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

    fn link_saturation(&self, _client: u32) -> f64 {
        self.saturation.get()
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

fn setup(what: &str, single_player: bool, sink: &Shared) -> Option<(World, Logic)> {
    let data = data(what)?;
    let config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    let world = World::new(data, config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, !single_player);
    logic.deliver_hook = Some(|_, _, _, _| {});
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn he02() -> u32 {
    create_id(b"he02")
}

/// A session in a system.
fn join(cx: &mut Cx<'_>, session: u32, system: u32) {
    let slot = cx.logic.players.get_mut(session).unwrap();
    slot.client_id = session;
    slot.system_id = system;
}

fn content(cx: &mut Cx<'_>) -> u32 {
    controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    )
    .0
}

/// A fighter spawned straight into the world (no server create).
fn fighter(cx: &mut Cx<'_>, system: u32, owner: u32, at: Vector) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    spawn.owner_player = owner;
    cx.world
        .spawn_ship(system, &spawn)
        .expect("he_fighter spawns")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

fn view(cx: &Cx<'_>, system: u32, player: u32) -> Observer {
    cx.logic
        .server_world
        .system(system)
        .unwrap()
        .observers
        .iter()
        .find(|o| o.player == player)
        .cloned()
        .expect("the observer")
}

fn set_ship(cx: &mut Cx<'_>, session: u32, ship: ObjHandle) {
    let ship_id = id(cx, ship);
    cx.logic.players.get_mut(session).unwrap().ship_object_id = ship_id;
}

fn server(message: &ToClient) -> Option<&ServerMessage> {
    match message {
        ToClient::Server(m) => Some(m),
        ToClient::Common(_) => None,
    }
}

fn created_ships(sent: &[(u32, ToClient)]) -> Vec<(u32, u32)> {
    sent.iter()
        .filter_map(|(c, m)| match server(m) {
            Some(ServerMessage::CreateShip(s)) => Some((*c, s.object.obj_id)),
            _ => None,
        })
        .collect()
}

fn destroyed(sent: &[(u32, ToClient)]) -> Vec<(u32, u32, u8)> {
    sent.iter()
        .filter_map(|(c, m)| match server(m) {
            Some(ServerMessage::DestroyObject(d)) => Some((*c, d.object_id, d.field_04)),
            _ => None,
        })
        .collect()
}

fn updates(sent: &[(u32, ToClient)]) -> Vec<(u32, u32, f32)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(CommonMessage::UpdateObject(u)) => {
                Some((*c, u.object_id, u.update_time))
            }
            _ => None,
        })
        .collect()
}

#[test]
fn visibility_at_each_range_and_for_the_owner() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("visibility", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    cx.world.load_system(he01());
    for session in 1..=3 {
        join(&mut cx, session, he01());
    }
    // Objects on the x = 0 plane; the viewer moves along x, so that each
    // squared distance is exact.
    let npc = fighter(&mut cx, he01(), 0, Vector::new(0.0, 3000.0, -25000.0));
    let other = fighter(&mut cx, he01(), 2, Vector::new(0.0, 6000.0, -25000.0));
    let own = fighter(&mut cx, he01(), 1, Vector::new(0.0, 9000.0, -25000.0));
    let seen = |cx: &Cx<'_>, h: ObjHandle, dx: f32| {
        let at = cx.world.get(h).unwrap().engine.position;
        let viewer = Vector::new(at.x + dx, at.y, at.z);
        replication::should_be_visible(cx.world, cx.logic, 1, viewer, h)
    };

    // Nobody's object: 2500 m, inclusive.
    assert!(seen(&cx, npc, 2499.0));
    assert!(seen(&cx, npc, 2500.0));
    assert!(!seen(&cx, npc, 2500.5));
    // Another player's, no group: 5000 m.
    assert!(seen(&cx, other, 5000.0));
    assert!(!seen(&cx, other, 5000.5));
    assert!(!seen(&cx, other, 9999.0));
    // The player's own object at any distance.
    assert!(seen(&cx, own, 1.0e7));

    // Players 1 and 2 in one group: 10 000 m for each other's objects,
    // still 2500 m for nobody's.
    let group = cx.logic.groups.create();
    cx.logic.players.get_mut(1).unwrap().group = Some(group);
    assert!(
        !seen(&cx, other, 5000.5),
        "only one of the two in the group"
    );
    cx.logic.players.get_mut(2).unwrap().group = Some(group);
    assert!(seen(&cx, other, 9999.0));
    assert!(seen(&cx, other, 10000.0));
    assert!(!seen(&cx, other, 10000.5));
    assert!(!seen(&cx, npc, 2500.5));
    // Player 3's own group is another one: the stranger radius again.
    let third = fighter(&mut cx, he01(), 3, Vector::new(0.0, 12000.0, -25000.0));
    let group3 = cx.logic.groups.create();
    cx.logic.players.get_mut(3).unwrap().group = Some(group3);
    assert!(seen(&cx, third, 5000.0));
    assert!(!seen(&cx, third, 5000.5));

    // A tracked object is visible exactly when it is created on the client.
    world::add_player(&mut cx, he01(), 1);
    let mut v = view(&cx, he01(), 1);
    assert_eq!(v.tracked.len(), 4, "add_player tracks the system's ships");
    assert!(v.tracked.iter().all(|t| !t.created_on_client));
    v.position = Vector::new(0.0, 3000.0, -25000.0);
    assert!(!replication::is_object_visible(
        cx.world,
        cx.logic,
        &v,
        Some(npc)
    ));
    v.tracked[0].created_on_client = true;
    assert!(replication::is_object_visible(
        cx.world,
        cx.logic,
        &v,
        Some(npc)
    ));
    assert!(replication::is_object_visible(
        cx.world,
        cx.logic,
        &v,
        Some(own)
    ));
    assert!(replication::is_object_visible(cx.world, cx.logic, &v, None));
}

#[test]
fn single_player_sees_everything_and_gets_every_create_at_once() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("single player's creates", true, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    join(&mut cx, 1, he02());
    // The client side built he02 in the shared world.
    cx.world.load_system(he02());
    cx.world.load_system_solars(he02());
    let far = Vector::new(0.0, 3000.0, 90000.0);
    let npc = fighter(&mut cx, he02(), 0, far);
    let own = fighter(&mut cx, he02(), 1, Vector::new(0.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own);
    let boron = create_id(b"commodity_boron");
    let loot = cx
        .world
        .spawn_loot(
            he02(),
            &LootSpawn {
                archetype: boron,
                contents: boron,
                units: 3,
                contents_hit_pts: 10.0,
                position: Vector::new(500.0, 3000.0, -25000.0),
                ..LootSpawn::default()
            },
        )
        .expect("a container");
    assert!(replication::should_be_visible(
        cx.world,
        cx.logic,
        1,
        Vector::ZERO,
        npc
    ));

    world::add_player(&mut cx, he02(), 1);
    let sent = sink.take();
    // No system id in single player; the NPC's create (not the player's own
    // ship's), then one CREATELOOT of every container.
    assert_eq!(sent.len(), 2, "{sent:?}");
    let Some(ServerMessage::CreateShip(ship)) = server(&sent[0].1) else {
        panic!("{sent:?}")
    };
    assert_eq!(sent[0].0, 1);
    let o = cx.world.get(npc).unwrap();
    let small = cx.world.data().archetypes.large_to_small(o.archetype);
    assert_eq!(ship.object.obj_id, o.simple.id);
    assert_eq!(ship.object.archetype, small);
    assert_eq!(ship.object.position, [far.x, far.y, far.z]);
    let q = matrix_to_quaternion(&o.engine.orientation);
    assert_eq!(
        ship.object.orientation,
        quantise_orientation([q.w, q.x, q.y, q.z])
    );
    assert_eq!(ship.object.hull, 255, "full health");
    assert!(ship.owner.is_none() && ship.names.0.is_empty() && !ship.marked_by);
    assert!(!ship.object.equipment.is_empty(), "the loadout");
    let Some(ServerMessage::CreateLoot(created)) = server(&sent[1].1) else {
        panic!("{sent:?}")
    };
    assert_eq!((created.owner_id, created.kind), (0, 5));
    assert_eq!(created.loot.len(), 1);
    assert_eq!(created.loot[0].loot_id, id(&cx, loot));
    assert_eq!(created.loot[0].units, 3);
    let v = view(&cx, he02(), 1);
    assert_eq!(
        v.tracked.iter().map(|t| t.object).collect::<Vec<_>>(),
        [npc, loot]
    );
    assert!(v.tracked.iter().all(|t| t.created_on_client));

    // A new ship goes out at once, from the creator's record.
    let made = space_obj::create(
        &mut cx,
        &ShipInfo::new(
            controller,
            he02(),
            create_id(b"he_fighter"),
            create_id(b"li_p_he_fighter_loadout01"),
            Vector::new(0.0, 3000.0, 80000.0),
            Matrix::IDENTITY,
        ),
    )
    .expect("the ship is made");
    let sent = sink.take();
    assert_eq!(created_ships(&sent), [(1, made)]);
    let Some(ServerMessage::CreateShip(ship)) = server(&sent[0].1) else {
        panic!("{sent:?}")
    };
    assert_eq!(
        ship.object.field_6c,
        create_id(b"li_p_he_fighter_loadout01"),
        "the request's loadout"
    );

    // No per-frame pass, and the clock does not run.
    world::step_system(&mut cx, he02(), 0.1);
    assert!(sink.take().is_empty());
    assert_eq!(view(&cx, he02(), 1).elapsed_time, 0.0);

    // A destroyed ship is destroyed on the client and forgotten.
    let npc_id = id(&cx, npc);
    world::destroy_object(&mut cx, npc, 0, 0);
    assert_eq!(destroyed(&sink.take()), [(1, npc_id, 0)]);
    assert_eq!(view(&cx, he02(), 1).tracked.len(), 2);
}

#[test]
fn a_multiplayer_frame_creates_in_range_and_destroys_out_of_range_or_dead() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the multiplayer pass", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    join(&mut cx, 1, he01());
    cx.world.load_system(he01());
    let npc = fighter(&mut cx, he01(), 0, Vector::new(15000.0, 3000.0, -25000.0));
    let npc_id = id(&cx, npc);
    world::add_player(&mut cx, he01(), 1);
    // The system id, and nothing of the ship yet: it is tracked.
    let sent = sink.take();
    assert!(created_ships(&sent).is_empty(), "{sent:?}");
    let v = view(&cx, he01(), 1);
    assert_eq!(v.tracked.len(), 1);
    assert!(!v.tracked[0].created_on_client);

    // The player's ships: one 3000 m away, one 1000 m away.
    let far = fighter(&mut cx, he01(), 1, Vector::new(18000.0, 3000.0, -25000.0));
    let near = fighter(&mut cx, he01(), 1, Vector::new(16000.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, far);
    world::step_system(&mut cx, he01(), 0.05);
    assert!(created_ships(&sink.take()).is_empty(), "out of range");
    let v = view(&cx, he01(), 1);
    assert_eq!(v.elapsed_time, f64::from(0.05f32));
    assert_eq!(v.position, cx.world.get(far).unwrap().engine.position);

    // In range: the create, and the record is created with a fresh
    // simulation.
    set_ship(&mut cx, 1, near);
    world::step_system(&mut cx, he01(), 0.05);
    let sent = sink.take();
    assert_eq!(created_ships(&sent), [(1, npc_id)], "{sent:?}");
    let v = view(&cx, he01(), 1);
    assert!(v.tracked[0].created_on_client);

    // Out of range again: destroyed on the client (kind 0) and uncreated.
    set_ship(&mut cx, 1, far);
    world::step_system(&mut cx, he01(), 0.05);
    assert_eq!(destroyed(&sink.take()), [(1, npc_id, 0)]);
    assert!(!view(&cx, he01(), 1).tracked[0].created_on_client);

    // Back in range: created again; then it dies, and the destroy carries
    // the kind; the record goes.
    set_ship(&mut cx, 1, near);
    world::step_system(&mut cx, he01(), 0.05);
    assert_eq!(created_ships(&sink.take()), [(1, npc_id)]);
    world::destroy_object(&mut cx, npc, 0, 0);
    assert_eq!(destroyed(&sink.take()), [(1, npc_id, 0)]);
    assert!(view(&cx, he01(), 1).tracked.is_empty());

    // A ship arriving from another system is tracked for the per-frame
    // pass; one that leaves is destroyed for the others (kind 0).
    cx.world.load_system(he02());
    let traveller = fighter(&mut cx, he02(), 0, Vector::new(0.0, 3000.0, -25000.0));
    assert!(world::remove_ship(&mut cx, traveller).is_some());
    let arrival = Vector::new(16500.0, 3000.0, -25000.0);
    assert!(world::add_ship(
        &mut cx,
        he01(),
        traveller,
        arrival,
        Matrix::IDENTITY
    ));
    assert!(sink.take().is_empty(), "tracked only");
    world::step_system(&mut cx, he01(), 0.05);
    let traveller_id = id(&cx, traveller);
    assert_eq!(created_ships(&sink.take()), [(1, traveller_id)]);
    assert!(world::remove_ship(&mut cx, traveller).is_some());
    assert_eq!(destroyed(&sink.take()), [(1, traveller_id, 0)]);
    assert!(view(&cx, he01(), 1).tracked.is_empty());
}

#[test]
fn updates_go_out_longest_without_one_first() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the update order", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    join(&mut cx, 1, he01());
    cx.world.load_system(he01());
    let ships: Vec<ObjHandle> = (0..3)
        .map(|i| {
            let x = 15000.0 + 300.0 * i as f32;
            fighter(&mut cx, he01(), 0, Vector::new(x, 3000.0, -25000.0))
        })
        .collect();
    let ids: Vec<u32> = ships.iter().map(|&h| id(&cx, h)).collect();
    world::add_player(&mut cx, he01(), 1);
    let own = fighter(&mut cx, he01(), 1, Vector::new(15300.0, 3500.0, -25000.0));
    set_ship(&mut cx, 1, own);
    world::step_system(&mut cx, he01(), 0.05);
    let sent = sink.take();
    assert_eq!(
        created_ships(&sent),
        ids.iter().map(|&i| (1, i)).collect::<Vec<_>>()
    );

    // Each simulation has gone a different time without an update and asks
    // for a sync (its back-off spent).
    {
        let v = &mut cx
            .logic
            .server_world
            .systems
            .get_mut(&he01())
            .unwrap()
            .observers[0];
        for (record, time) in v.tracked.iter_mut().zip([1.0, 3.0, 2.0]) {
            record.simulation.simulation_time = time;
            record.simulation.sync_backoff = 0.0;
        }
    }
    world::step_system(&mut cx, he01(), 0.05);
    let sent = updates(&sink.take());
    assert_eq!(
        sent,
        [
            (1, ids[1], (f64::from(0.05f32) * 2.0) as f32),
            (1, ids[2], (f64::from(0.05f32) * 2.0) as f32),
            (1, ids[0], (f64::from(0.05f32) * 2.0) as f32),
        ]
    );
    // Each update fed the record's own simulation.
    let v = view(&cx, he01(), 1);
    assert!(
        v.tracked
            .iter()
            .all(|t| t.simulation.simulation_time == 0.0)
    );

    // The update carries the ship's position, orientation and throttle,
    // and the behaviour byte (none: 0xff).
    {
        let v = &mut cx
            .logic
            .server_world
            .systems
            .get_mut(&he01())
            .unwrap()
            .observers[0];
        v.tracked[0].simulation.sync_backoff = -1.0;
    }
    world::step_system(&mut cx, he01(), 0.05);
    let update = sink
        .take()
        .into_iter()
        .find_map(|(_, m)| match m {
            ToClient::Common(CommonMessage::UpdateObject(u)) if u.object_id == ids[0] => Some(u),
            _ => None,
        })
        .expect("the update");
    let after = cx.world.get(ships[0]).unwrap();
    let q = matrix_to_quaternion(&after.engine.orientation);
    assert_eq!(
        update.position,
        [
            after.engine.position.x,
            after.engine.position.y,
            after.engine.position.z
        ]
    );
    assert_eq!(
        update.orientation,
        quantise_orientation([q.w, q.x, q.y, q.z])
    );
    assert_eq!((update.flags, update.extra), (0xff, 0));
}

#[test]
fn a_saturated_link_stops_the_pass() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the saturation gate", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    join(&mut cx, 1, he01());
    cx.world.load_system(he01());
    let npc = fighter(&mut cx, he01(), 0, Vector::new(15000.0, 3000.0, -25000.0));
    world::add_player(&mut cx, he01(), 1);
    let own = fighter(&mut cx, he01(), 1, Vector::new(15500.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own);
    let _ = sink.take();
    sink.saturation.set(0.9);
    world::step_system(&mut cx, he01(), 0.05);
    assert!(sink.take().is_empty(), "0.9 stops every phase");
    let v = view(&cx, he01(), 1);
    assert!(!v.tracked[0].created_on_client);
    assert_eq!(v.elapsed_time, f64::from(0.05f32), "the clock still runs");
    sink.saturation.set(0.899_999);
    world::step_system(&mut cx, he01(), 0.05);
    assert_eq!(created_ships(&sink.take()), [(1, id(&cx, npc))]);
}

#[test]
fn creators_tell_the_players_and_damage_waits_until_visible() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the creators' fan-outs", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    join(&mut cx, 1, he01());
    join(&mut cx, 2, he01());
    cx.world.load_system(he01());
    world::add_player(&mut cx, he01(), 1);
    world::add_player(&mut cx, he01(), 2);
    let own = fighter(&mut cx, he01(), 1, Vector::new(15000.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own);
    world::step_system(&mut cx, he01(), 0.05);
    let _ = sink.take();

    // `pub::SpaceObj::Create` in multiplayer: tracked by both players,
    // created on neither until the frame.
    let made = space_obj::create(
        &mut cx,
        &ShipInfo::new(
            controller,
            he01(),
            create_id(b"he_fighter"),
            create_id(b"li_p_he_fighter_loadout01"),
            Vector::new(15800.0, 3000.0, -25000.0),
            Matrix::IDENTITY,
        ),
    )
    .expect("the ship is made");
    assert!(created_ships(&sink.take()).is_empty());
    assert!(
        view(&cx, he01(), 2)
            .find(cx.world.find(made).unwrap())
            .is_some()
    );

    // A mine in range of player 1: created at once for player 1 only
    // (player 2 has no ship, so it views from the origin).
    let own_id = id(&cx, own);
    let mine = munitions::create_mine(
        &mut cx,
        he01(),
        &ProjectileSpawn {
            archetype: create_id(b"mine01_mark01_ammo"),
            owner: own_id,
            position: Vector::new(15000.0, 3000.0, -24900.0),
            orientation: Matrix::IDENTITY,
            velocity: Vector::ZERO,
            target: None,
            angular_velocity: Vector::ZERO,
        },
    )
    .expect("the mine");
    let mine_id = id(&cx, mine);
    let sent = sink.take();
    let mines: Vec<(u32, u32, u32)> = sent
        .iter()
        .filter_map(|(c, m)| match server(m) {
            Some(ServerMessage::CreateMine(x)) => Some((*c, x.obj_id, x.owner_id)),
            _ => None,
        })
        .collect();
    assert_eq!(mines, [(1, mine_id, id(&cx, own))]);
    assert!(view(&cx, he01(), 1).find(mine).unwrap().created_on_client);
    assert!(!view(&cx, he01(), 2).find(mine).unwrap().created_on_client);

    // A solar's damage out of sight waits for the fifth phase; once the
    // player can see it, its whole state goes out (cause 0x19).
    let solar = cx
        .world
        .objects_in(he01())
        .into_iter()
        .find(|&h| {
            cx.world.get(h).is_some_and(|o| {
                o.kind == game_sim::object::ObjectKind::Solar
                    && o.simple.type_bits & replication::TRADELANE_RING == 0
                    && o.simple.max_hit_pts > 0.0
                    && o.simple.max_hit_pts < 1.0e30
            })
        })
        .expect("a solar with hit points");
    let solar_id = id(&cx, solar);
    let at = cx.world.get(solar).unwrap().engine.position;
    let max = cx.world.get(solar).unwrap().simple.max_hit_pts;
    let mut list = DamageList::new(1, 0, 0);
    let hit = (f64::from(max) * 0.5) as f32;
    list.add_damage_entry(SUBOBJ_ID_ROOT, hit, Fate::None);
    {
        let mut hooks = Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.apply_damage_list_hooked(solar, &list, &mut hooks);
    }
    let sent = sink.take();
    assert!(
        !sent
            .iter()
            .any(|(c, m)| *c == 1 && matches!(server(m), Some(ServerMessage::DamageObject(_)))),
        "{sent:?}"
    );
    assert!(view(&cx, he01(), 1).pending_damage.contains(&solar_id));
    // The player's ship next to the solar (the radius is 2500 m).
    let close = fighter(&mut cx, he01(), 1, Vector::new(at.x, at.y + 2000.0, at.z));
    set_ship(&mut cx, 1, close);
    world::step_system(&mut cx, he01(), 0.05);
    let damage = sink
        .take()
        .into_iter()
        .find_map(|(c, m)| match m {
            ToClient::Server(ServerMessage::DamageObject(d)) if c == 1 => Some(d),
            _ => None,
        })
        .expect("the deferred damage");
    assert_eq!(damage.object_id, solar_id);
    assert_eq!((damage.damage.cause, damage.damage.destroyed), (0x19, 0));
    assert_eq!(damage.damage.entries[0].sub_obj_id, SUBOBJ_ID_ROOT);
    assert_eq!(damage.damage.entries[0].hit_points, hit);
    assert!(view(&cx, he01(), 1).pending_damage.is_empty());
}
