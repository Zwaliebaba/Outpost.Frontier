//! `StarSystem_UpdateNearShips` over the solar BSP on the original data
//! (`outdir/`, the rebranded he01): the station he01_14 updates as often
//! per sub-step as the C++ updates it with one, two and three players
//! near it and with entries of the always-updated list (`ListD4`).
//!
//! The C++ (`Server/IServerImpl.cpp:3638`) appends the BSP's answers for
//! a 10000 m sphere around each player's ship to one scan list of at most
//! 0x800 entries, then the `ListD4` entries, and its de-duplication drops
//! only the first later repeat of each item: the station listed `n` times
//! updates `ceil(n / 2)` times. Each update of the station is counted by
//! `EqObj::update`'s cargo decay timer, which gathers the step's time.

use std::path::Path;
use std::sync::Arc;

use game_logic::server::{ClientSink, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

struct Sink;

impl ClientSink for Sink {
    fn send(&mut self, _client: u32, _message: ToClient) {}
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no near-solar pass");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let config = WorldConfig {
        single_player: false,
        ..WorldConfig::default()
    };
    let world = World::new(Arc::new(data), config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(Sink), 4, true);
    logic.deliver_hook = Some(|_, _, _, _| {});
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn fighter(cx: &mut Cx<'_>, owner: u32, at: Vector) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    spawn.owner_player = owner;
    cx.world
        .spawn_ship(he01(), &spawn)
        .expect("he_fighter spawns")
}

fn set_ship(cx: &mut Cx<'_>, session: u32, ship: ObjHandle) {
    let ship_id = cx.world.get(ship).unwrap().simple.id;
    cx.logic.players.get_mut(session).unwrap().ship_object_id = ship_id;
}

/// The station's updates in one frame of one sub-step.
fn updates(cx: &mut Cx<'_>, station: ObjHandle) -> u32 {
    const DT: f32 = 0.05;
    cx.logic
        .objects
        .entry(cx.world, station)
        .unwrap()
        .cargo_decay_timer = 0.0;
    assert_eq!(world::step_system(cx, he01(), DT), 1);
    let gathered = cx.logic.objects.get(station).unwrap().cargo_decay_timer;
    let count = (f64::from(gathered) / f64::from(DT)).round();
    assert!((0.0..100.0).contains(&count), "{gathered}");
    count as u32
}

/// The station's entries in the scan list the BSP fills.
fn listed(cx: &Cx<'_>, station: ObjHandle) -> usize {
    world::near_solars(cx.world, cx.logic, he01())
        .expect("he01 has its BSP")
        .iter()
        .filter(|&&h| h == station)
        .count()
}

#[test]
fn a_solar_updates_as_often_as_the_cpp_scan_list_says() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for session in 1..=3 {
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = he01();
    }
    world::load(&mut cx, he01());
    let station = cx
        .world
        .find(create_id(b"he01_14"))
        .expect("he01_14 is loaded");
    let at = cx.world.get(station).unwrap().engine.position;
    let radius = cx.world.get(station).unwrap().engine.radius;
    let offset = |x: f32| Vector::new(at.x + x, at.y + 300.0, at.z);
    // Each player has a ship near the station (within the sphere) and one
    // whose sphere does not reach the station's.
    let mut near = Vec::new();
    let mut far = Vec::new();
    for session in 1..=3u32 {
        let step = session as f32 * 200.0;
        near.push(fighter(&mut cx, session, offset(radius + 1500.0 + step)));
        far.push(fighter(
            &mut cx,
            session,
            offset(-(radius * 2.0 + 10500.0 + step)),
        ));
    }
    for session in 1..=3u32 {
        world::add_player(&mut cx, he01(), session);
        set_ship(&mut cx, session, far[session as usize - 1]);
    }

    // No player near: the station is not listed and does not update.
    assert_eq!(listed(&cx, station), 0);
    assert_eq!(updates(&mut cx, station), 0);

    // One, two and three players near: listed once per player, updated
    // once, once and twice.
    let mut expected = Vec::new();
    let mut got = Vec::new();
    for players in 1..=3u32 {
        set_ship(&mut cx, players, near[players as usize - 1]);
        assert_eq!(listed(&cx, station), players as usize);
        expected.push(players.div_ceil(2));
        got.push(updates(&mut cx, station));
    }
    assert_eq!(got, expected);
    assert_eq!(got, [1, 1, 2]);

    // A queued dock request's `ListD4` entry follows the BSP's: two
    // players and one entry list the station three times (two updates),
    // three players and one entry four times (two updates).
    cx.world.add_always_update(station);
    set_ship(&mut cx, 3, far[2]);
    assert_eq!(updates(&mut cx, station), 2);
    set_ship(&mut cx, 3, near[2]);
    assert_eq!(updates(&mut cx, station), 2);
    // One player and two entries: three times, two updates.
    set_ship(&mut cx, 2, far[1]);
    set_ship(&mut cx, 3, far[2]);
    cx.world.add_always_update(station);
    assert_eq!(updates(&mut cx, station), 2);
    // Docked players (no ship) query nothing; the entries alone list it
    // twice, one update.
    for session in 1..=3 {
        cx.logic.players.get_mut(session).unwrap().ship_object_id = 0;
    }
    assert_eq!(listed(&cx, station), 0);
    assert_eq!(updates(&mut cx, station), 1);
    cx.world.remove_always_update(station);
    cx.world.remove_always_update(station);
    assert_eq!(updates(&mut cx, station), 0);
}

#[test]
fn a_destroyed_solar_leaves_the_bsp() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let slot = cx.logic.players.get_mut(1).unwrap();
    slot.client_id = 1;
    slot.system_id = he01();
    world::load(&mut cx, he01());
    let station = cx
        .world
        .find(create_id(b"he01_14"))
        .expect("he01_14 is loaded");
    let at = cx.world.get(station).unwrap().engine.position;
    let ship = fighter(&mut cx, 1, Vector::new(at.x + 2000.0, at.y, at.z));
    world::add_player(&mut cx, he01(), 1);
    set_ship(&mut cx, 1, ship);
    let before = world::near_solars(cx.world, cx.logic, he01()).unwrap();
    assert!(before.contains(&station));
    // `Solar::destroy` (`StarSystem::remove_solar`): out of the BSP at
    // once, the others still found.
    world::destroy_object(&mut cx, station, 0, 0);
    let after = world::near_solars(cx.world, cx.logic, he01()).unwrap();
    assert!(!after.contains(&station));
    let mut rest: Vec<ObjHandle> = before.into_iter().filter(|&h| h != station).collect();
    let mut found = after;
    rest.sort();
    found.sort();
    assert_eq!(found, rest);
}
