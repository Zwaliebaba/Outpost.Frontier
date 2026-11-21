//! P3.3's `pub::` families over the original data (`outdir/`):
//! `pub::Reputation` (faction feelings from `InitialWorld.ini`, run-time
//! vibes, attitudes, ranks and the notices), `pub::Zone` (he01's zones by
//! position), `pub::System` (he01's name, zones, connections, objects and
//! solars), `pub::Phantom`, `pub::Controller`, `pub::Audio` and the
//! top-level system getters. The expected values follow the C++ logic of
//! `Server/server_pub.cpp` and Common.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::{
    PubError, audio, controller, misc, phantom, reputation as rep, system, zone,
};
use game_logic::events::{ControllerMessage, PhantomKey};
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::reputation::{MISSING_GROUP, group_id};
use game_sim::data::zone::ZoneShape;
use game_sim::data::zone::query::Sphere;
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::vfs::Host;
use neuron_server::controller::{ControllerId, Priority};

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

thread_local! {
    static HEARD: RefCell<Vec<(u32, u32, u32, bool)>> = const { RefCell::new(Vec::new()) };
    static BUSY: RefCell<Vec<i32>> = const { RefCell::new(Vec::new()) };
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn data() -> Option<Arc<GameData>> {
    let config = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    Some(Arc::new(data))
}

fn new_world(data: Arc<GameData>, config: WorldConfig) -> World {
    let mut world = World::new(data, config).with_assets(Assets::new(outdir().join("data")));
    // The faction feelings of `InitialWorld.ini` (`Reputation::LoadFeelings`).
    let text = std::fs::read(outdir().join("data/initialworld.ini")).expect("InitialWorld.ini");
    let mut reader = IniReader::new();
    assert!(reader.open_memory(&text));
    let mut reports = game_sim::data::Reports::default();
    world
        .reputation_mut()
        .load_feelings(&mut reader, &mut reports);
    world
}

/// `QuantizeAttitude` of the vibe store.
fn quantize(value: f64) -> f32 {
    let step = 100.0;
    let half = 0.5 / step;
    let epsilon = 1.0e-8;
    if value < -epsilon {
        (f64::from(neuron_core::num::trunc_to_i32((value - half) * step)) / step - epsilon) as f32
    } else if value > epsilon {
        (f64::from(neuron_core::num::trunc_to_i32((value + half) * step)) / step + epsilon) as f32
    } else {
        0.0
    }
}

fn names(a: u32, b: u32) -> (FmtStr, FmtStr) {
    (FmtStr::new(a), FmtStr::new(b))
}

#[test]
fn pub_families_over_the_data() {
    let Some(data) = data() else {
        println!("outdir/ is missing: no pub:: families");
        return;
    };
    reputation_multiplayer(Arc::clone(&data));
    reputation_single_player(Arc::clone(&data));
    zones(Arc::clone(&data));
    systems(Arc::clone(&data));
    phantoms_controllers_and_audio(data);
}

fn reputation_multiplayer(data: Arc<GameData>) {
    let mut world = new_world(data, WorldConfig::default());
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 4, true);
    logic.players.get_mut(1).unwrap().client_id = 1;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };

    // Factions by nickname, and their feelings from `InitialWorld.ini`.
    let he_n = group_id(b"he_n_grp");
    let he_p = group_id(b"he_p_grp");
    let ve_n = group_id(b"ve_n_grp");
    let or_n = group_id(b"or_n_grp");
    assert_eq!(rep::reputation_group(&cx, b"he_n_grp"), Ok(he_n));
    assert_eq!(
        rep::reputation_group(&cx, b"he_n_grp_and_more"),
        Err(PubError::BAD_ID),
        "cut to 15 bytes: another name"
    );
    assert_eq!(rep::reputation_group(&cx, b""), Err(PubError::BAD_ID));
    assert_eq!(rep::reputation_group(&cx, b"nobody"), Err(PubError::BAD_ID));
    assert_eq!(rep::group_reputation(&cx, he_n, ve_n), Ok(0.65));
    assert_eq!(rep::group_reputation(&cx, ve_n, he_n), Ok(0.65));
    assert_eq!(rep::group_reputation(&cx, he_n, he_n), Ok(1.0));
    assert_eq!(rep::group_reputation(&cx, he_p, or_n), Ok(0.0));
    assert_eq!(rep::group_reputation(&cx, he_n, or_n), Ok(0.65));
    assert_eq!(
        rep::group_reputation(&cx, MISSING_GROUP, he_n),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        rep::group_reputation(&cx, 0x1234, he_n),
        Err(PubError::BAD_ID)
    );
    assert_eq!(rep::group_name(&cx, he_n), Ok(196_846));
    assert_eq!(rep::short_group_name(&cx, he_n), Ok(196_895));
    assert_eq!(rep::group_name(&cx, 0x1234), Ok(0), "get_name answers 0");

    // `SetReputation(group, group)`: `affect_relation` by the float
    // difference; a multiplayer server sends no session-0 notice.
    let base = 0.65f32;
    assert_eq!(rep::set_group_reputation(&mut cx, he_n, ve_n, 0.2), Ok(()));
    let delta = (f64::from(0.2f32) - f64::from(base)) as f32;
    let expected = (f64::from(delta) + f64::from(base)) as f32;
    assert_eq!(rep::group_reputation(&cx, he_n, ve_n), Ok(expected));
    assert_eq!(rep::group_reputation(&cx, ve_n, he_n), Ok(0.65), "one way");
    assert_eq!(
        rep::set_group_reputation(&mut cx, he_n, u32::MAX, 0.2),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        rep::set_group_reputation(&mut cx, he_n, 0x1234, 0.2),
        Err(PubError::BAD_ID)
    );
    assert!(sink.take().is_empty());

    // The groups in id order, until the callback says stop.
    let mut ids = Vec::new();
    assert_eq!(
        rep::enumerate_groups(&cx, |id| {
            ids.push(id);
            true
        }),
        0
    );
    assert_eq!(ids.len(), cx.world.reputation().group_count());
    assert!(ids.windows(2).all(|w| w[0] < w[1]) && ids.contains(&he_n));
    let mut seen = 0;
    rep::enumerate_groups(&cx, |_| {
        seen += 1;
        seen < 3
    });
    assert_eq!(seen, 3);

    // Run-time vibes: ids from the players' map, starting after 0.
    let (n1, n2) = names(11, 12);
    let a = rep::alloc(&mut cx, &n1, &n2).expect("a vibe");
    let b = rep::alloc(&mut cx, &FmtStr::new(21), &FmtStr::new(22)).expect("a vibe");
    assert_eq!((a, b), (1, 2));
    assert_eq!(rep::name(&mut cx, a), Ok((n1, n2)));
    assert_eq!(rep::name(&mut cx, 999), Err(PubError::BAD_ID));
    assert_eq!(rep::affiliation(&mut cx, a), Ok(MISSING_GROUP));
    assert_eq!(rep::affiliation(&mut cx, 999), Err(PubError::BAD_ID));
    assert_eq!(rep::set_affiliation(&mut cx, a, he_n), Ok(()));
    assert_eq!(rep::set_affiliation(&mut cx, b, ve_n), Ok(()));
    assert_eq!(rep::affiliation(&mut cx, a), Ok(he_n));
    assert_eq!(
        rep::set_affiliation(&mut cx, a, 0x1234),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        rep::set_affiliation(&mut cx, 999, he_n),
        Err(PubError::BAD_ID)
    );

    // A vibe's feeling towards a group: the group's feeling towards the
    // vibe's faction, plus the vibe's own override.
    assert_eq!(rep::reputation(&mut cx, a, ve_n), Ok(0.65));
    assert_eq!(rep::reputation(&mut cx, 999, ve_n), Err(PubError::BAD_ID));
    assert_eq!(rep::reputation(&mut cx, a, 0x1234), Err(PubError::BAD_ID));
    assert_eq!(rep::set_reputation(&mut cx, a, ve_n, 0.3), Ok(()));
    let over = (f64::from(0.3f32) - f64::from(0.65f32)) as f32;
    let expected = (f64::from(over) + f64::from(0.65f32)) as f32;
    assert_eq!(rep::reputation(&mut cx, a, ve_n), Ok(expected));
    assert_eq!(rep::group_feelings_towards(&mut cx, a, ve_n), Ok(expected));
    assert_eq!(
        rep::group_feelings_towards(&mut cx, 999, ve_n),
        Ok(0.0),
        "only -1 fails, and the store never answers it"
    );
    assert_eq!(
        rep::set_reputation(&mut cx, 999, ve_n, 0.3),
        Err(PubError::BAD_ID)
    );

    // Attitudes: the cached faction's feeling towards the other's faction,
    // quantized; unknown vibes give -3 either way.
    let feeling = cx
        .world
        .reputation()
        .feelings_towards(he_n, ve_n)
        .1
        .unwrap();
    assert_eq!(
        rep::attitude(&mut cx, a, b),
        Ok(quantize(f64::from(feeling)))
    );
    assert_eq!(rep::attitude(&mut cx, 999, b), Err(PubError::NOT_FOUND));
    assert_eq!(rep::attitude(&mut cx, a, 999), Err(PubError::NOT_FOUND));
    assert_eq!(rep::set_attitude(&mut cx, a, b, 0.1), Ok(()));
    let base = f64::from(feeling).clamp(-1.0, 1.0);
    let stored = (f64::from(0.1f32) - base) as f32;
    assert_eq!(
        rep::attitude(&mut cx, a, b),
        Ok(quantize(f64::from(feeling) + f64::from(stored)))
    );
    assert_eq!(
        rep::set_attitude(&mut cx, 999, b, 0.1),
        Err(PubError::NOT_FOUND)
    );
    assert!(sink.take().is_empty(), "b has no client");

    // `SetAttitude` towards a player's vibe tells that session.
    cx.world.vibes().0.set_client_id(b, 1);
    cx.logic.players.get_mut(1).unwrap().rep_vibe = b;
    assert_eq!(rep::set_attitude(&mut cx, a, b, -0.5), Ok(()));
    let sent = sink.take();
    let attitude = rep::attitude(&mut cx, a, b).unwrap();
    match &sent[..] {
        [(1, ServerMessage::SetReputation(r))] => {
            assert_eq!(r.target, a as u32);
            assert_eq!(r.affiliation, 0);
            assert_eq!(r.attitude, attitude);
            assert!(r.relations.is_empty() && r.display_name.is_empty());
        }
        other => panic!("{other:?}"),
    }

    // Ranks: a float of the unsigned rank; `SetRank` truncates.
    assert_eq!(rep::rank(&mut cx, a), Ok(0.0));
    assert_eq!(rep::set_rank(&mut cx, a, 7.9), Ok(()));
    assert_eq!(rep::rank(&mut cx, a), Ok(7.0));
    assert_eq!(rep::rank(&mut cx, 999), Err(PubError::BAD_ID));
    assert_eq!(rep::set_rank(&mut cx, 999, 1.0), Err(PubError::BAD_ID));
    assert!(sink.take().is_empty());
    // A player's vibe: the old rank is read from the vibe whose key is the
    // client id (1, vibe `a`, rank 7), so 7 counts as no change.
    assert_eq!(rep::set_rank(&mut cx, b, 7.5), Ok(()));
    assert!(sink.take().is_empty(), "same as vibe 1's rank");
    assert_eq!(rep::set_rank(&mut cx, b, 3.0), Ok(()));
    match &sink.take()[..] {
        [(1, ServerMessage::MiscObjUpdate(u))] => {
            assert_eq!((u.field_24, u.field_2c), (1, 3));
        }
        other => panic!("{other:?}"),
    }

    // Solar vibes by nickname: every system object has one, with no
    // faction at load (fact F17).
    let planet = rep::solar_reputation_by_name(&mut cx, b"he01_01").expect("he01_01's vibe");
    assert_eq!(planet, create_id(b"he01_01") as i32);
    assert_eq!(rep::solar_reputation(&mut cx, planet as u32), Ok(planet));
    assert_eq!(
        rep::solar_reputation_by_name(&mut cx, b"no_such_solar"),
        Err(PubError::BAD_ID)
    );
    assert_eq!(rep::affiliation(&mut cx, planet), Ok(MISSING_GROUP));
    assert_eq!(rep::reputation(&mut cx, planet, he_n), Ok(0.0));

    // `Free`: only a dynamic vibe; its id goes back to the map.
    assert_eq!(rep::free(&mut cx, b), Ok(()));
    assert_eq!(rep::free(&mut cx, b), Err(PubError::BAD_ID));
    assert_eq!(rep::free(&mut cx, planet), Err(PubError::BAD_ID));
    assert_eq!(rep::free(&mut cx, 0x5000), Err(PubError::BAD_ID));
    assert_eq!(rep::alloc(&mut cx, &FmtStr::new(1), &FmtStr::new(2)), Ok(3));
}

fn reputation_single_player(data: Arc<GameData>) {
    let config = WorldConfig {
        single_player: true,
        server_physics: false,
        ..WorldConfig::default()
    };
    let mut world = new_world(data, config);
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, false);
    logic.players.get_mut(1).unwrap().client_id = 7;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he_n = group_id(b"he_n_grp");
    let ve_n = group_id(b"ve_n_grp");

    // Every change sends `Vibe::Get` of the vibe to every active client.
    let (n1, n2) = names(31, 32);
    let a = rep::alloc(&mut cx, &n1, &n2).unwrap();
    let flat = |f: &FmtStr| f.flatten().unwrap();
    match &sink.take()[..] {
        [(7, ServerMessage::SetReputation(r))] => {
            assert_eq!(r.target, a as u32);
            assert_eq!(r.affiliation, MISSING_GROUP);
            assert_eq!(r.names, (flat(&n1), flat(&n2)));
            assert!(r.relations.is_empty());
        }
        other => panic!("{other:?}"),
    }
    assert_eq!(rep::set_affiliation(&mut cx, a, he_n), Ok(()));
    assert_eq!(rep::set_reputation(&mut cx, a, ve_n, -0.25), Ok(()));
    let sent = sink.take();
    assert_eq!(sent.len(), 2);
    match &sent[1] {
        (7, ServerMessage::SetReputation(r)) => {
            assert_eq!(r.affiliation, he_n);
            assert_eq!(r.relations.len(), 1);
            assert_eq!(r.relations[0].group_id, ve_n);
            let over = (f64::from(-0.25f32) - f64::from(0.65f32)) as f32;
            assert_eq!(r.relations[0].feeling, over);
        }
        other => panic!("{other:?}"),
    }
    // The group pair's notice names the vibe whose key is the group id:
    // none, so the affiliation is 0 and the rest empty.
    assert_eq!(rep::set_group_reputation(&mut cx, he_n, ve_n, 0.5), Ok(()));
    match &sink.take()[..] {
        [(7, ServerMessage::SetReputation(r))] => {
            assert_eq!((r.target, r.affiliation), (he_n, 0));
            assert!(r.relations.is_empty());
        }
        other => panic!("{other:?}"),
    }
    // Single player sends no attitude or rank notices.
    let b = rep::alloc(&mut cx, &FmtStr::new(1), &FmtStr::new(2)).unwrap();
    cx.world.vibes().0.set_client_id(b, 1);
    sink.take();
    assert_eq!(rep::set_attitude(&mut cx, a, b, 0.5), Ok(()));
    assert_eq!(rep::set_rank(&mut cx, b, 4.0), Ok(()));
    assert!(sink.take().is_empty());
}

fn zones(data: Arc<GameData>) {
    let mut world = new_world(data, WorldConfig::default());
    let mut logic = Logic::new(Box::new(Shared::default()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let death = create_id(b"Zone_he01_Manhattan_death");
    let burn = create_id(b"Zone_he01_001_Planet_he01_01_atmosphere_burn");
    let planet = create_id(b"Zone_he01_001_Planet_he01_01");
    let centre = Vector::new(-33270.0, 0.0, -33039.0);

    assert_eq!(zone::id(&mut cx, he01, b"Zone_he01_Manhattan_death"), death);
    assert_eq!(zone::id(&mut cx, 0, b"zone_he01_manhattan_death"), death);
    assert!(zone::verify_id(&mut cx, death));
    let before = cx.logic.reports.0.len();
    assert!(!zone::verify_id(&mut cx, 12345));
    assert_eq!(zone::id(&mut cx, he01, b"no_such_zone"), 0);
    assert_eq!(cx.logic.reports.0.len(), before + 2, "both misses reported");
    assert_eq!(
        cx.logic.reports.0[before].text,
        "*** WARNING: get_zone(12345) failed"
    );

    // The death zone: a sphere of 4100 m round the planet.
    assert_eq!(zone::system(&mut cx, death), he01);
    assert_eq!(zone::pos(&mut cx, death), centre);
    assert_eq!(zone::orientation(&mut cx, death), Ok(Matrix::IDENTITY));
    assert_eq!(zone::radius(&mut cx, death), 4100.0);
    assert_eq!(zone::shape(&mut cx, death), Ok(ZoneShape::Sphere));
    assert_eq!(zone::size(&mut cx, death).map(|s| s.x), Ok(4100.0));
    assert!(zone::properties(&mut cx, death).is_ok());
    assert_eq!(zone::name(&mut cx, death), Ok(0));
    assert_eq!(
        zone::lootable_info(&mut cx, death),
        Ok(zone::LootableInfo::default())
    );
    assert_eq!(zone::population(&mut cx, death), Ok(Vec::new()));
    let population = zone::population(&mut cx, planet).unwrap();
    assert_eq!(population.first(), Some(&(group_id(b"he_p_grp"), 1.0)));

    // By position.
    let half_way = Vector::new(centre.x + 2050.0, 0.0, centre.z);
    let outside = Vector::new(centre.x + 5000.0, 0.0, centre.z);
    assert_eq!(zone::classify_point(&mut cx, death, centre), 0.0);
    assert_eq!(zone::classify_point(&mut cx, death, half_way), 0.5);
    assert!(zone::in_zone(&mut cx, death, centre, 0.0));
    assert!(zone::in_zone(&mut cx, death, half_way, 0.0));
    assert!(!zone::in_zone(&mut cx, death, outside, 0.0));
    assert!(zone::in_zone(
        &mut cx,
        burn,
        Vector::new(centre.x + 4500.0, 0.0, centre.z),
        0.0
    ));
    assert_eq!(zone::distance(&mut cx, death, centre), -4100.0);
    assert_eq!(zone::distance(&mut cx, death, outside), 900.0);
    let far = Vector::new(centre.x + 10000.0, 0.0, centre.z);
    let hit = zone::intersect(&mut cx, death, far, centre).expect("enters");
    assert!((hit.x - (centre.x + 4100.0)).abs() < 0.5, "{hit:?}");
    assert_eq!(
        zone::intersect(&mut cx, death, far, Vector::new(far.x, 0.0, far.z + 1.0)),
        None
    );

    // An unknown zone answers the C++'s defaults.
    assert_eq!(zone::system(&mut cx, 12345), 0);
    assert_eq!(zone::pos(&mut cx, 12345), Vector::ZERO);
    assert_eq!(zone::radius(&mut cx, 12345), 0.0);
    assert_eq!(zone::classify_point(&mut cx, 12345, centre), 1.0);
    assert_eq!(zone::distance(&mut cx, 12345, centre), 0.0);
    assert!(!zone::in_zone(&mut cx, 12345, centre, 0.0));
    assert_eq!(zone::intersect(&mut cx, 12345, far, centre), None);
    assert_eq!(zone::shape(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::size(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::orientation(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::properties(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::population(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::lootable_info(&mut cx, 12345), Err(PubError::BAD_ID));
    assert_eq!(zone::name(&mut cx, 12345), Err(PubError::BAD_ID));

    // A zone an asteroid field gave loot, if he01 has one: the record in
    // the C++'s field order.
    let looted = cx
        .world
        .data()
        .universe
        .system_zones(he01)
        .find_map(|z| z.lootable.map(|l| (z.nickname, l)));
    if let Some((id, l)) = looted {
        let info = zone::lootable_info(&mut cx, id).unwrap();
        assert_eq!(
            (
                info.archetype,
                info.loot_table,
                info.min_level,
                info.max_level,
                info.field6
            ),
            (
                l.asteroid_loot_commodity,
                l.dynamic_loot_commodity,
                l.asteroid_loot_difficulty,
                l.asteroid_loot_count_min,
                l.asteroid_loot_count_max
            )
        );
    }
}

fn systems(data: Arc<GameData>) {
    let mut world = new_world(data, WorldConfig::default());
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let source = Host::new(outdir().join("data"));
    let he01 = create_id(b"he01");
    let he02 = create_id(b"he02");
    let unknown = create_id(b"no_such_system");
    let centre = Vector::new(-33270.0, 0.0, -33039.0);
    let death = create_id(b"Zone_he01_Manhattan_death");
    let burn = create_id(b"Zone_he01_001_Planet_he01_01_atmosphere_burn");
    let planet_zone = create_id(b"Zone_he01_001_Planet_he01_01");

    // The top-level getters.
    assert_eq!(misc::system_id(&mut cx, b"he01"), Ok(he01));
    assert_eq!(misc::system_id(&mut cx, b"HE01"), Ok(he01));
    assert_eq!(misc::system_id(&mut cx, b""), Err(PubError::BAD_ARGUMENT));
    let before = cx.logic.reports.0.len();
    assert_eq!(
        misc::system_id(&mut cx, b"no_such_system"),
        Err(PubError::BAD_ID)
    );
    assert_eq!(cx.logic.reports.0.len(), before + 1);
    let keys: Vec<u32> = cx.world.data().universe.systems.keys().copied().collect();
    let mut id = 0;
    assert!(misc::next_system_id(&cx, &mut id));
    assert_eq!(id, keys[0]);
    let at = keys.iter().position(|&k| k == he01).unwrap();
    let mut id = he01;
    assert_eq!(misc::next_system_id(&cx, &mut id), at + 1 < keys.len());
    if at + 1 < keys.len() {
        assert_eq!(id, keys[at + 1]);
    }
    let mut id = *keys.last().unwrap();
    assert!(!misc::next_system_id(&cx, &mut id));
    assert_eq!(id, *keys.last().unwrap());
    let mut id = unknown;
    assert!(
        !misc::next_system_id(&cx, &mut id),
        "not found: off the end"
    );
    assert_eq!(misc::nickname_id(b"he01"), he01);
    assert_eq!(misc::make_id(b"he_n_grp") as u32, group_id(b"he_n_grp"));
    assert_eq!(
        misc::system_nickname(&mut cx, 64, he01),
        (0, Some(b"he01".to_vec()))
    );
    assert_eq!(misc::system_nickname(&mut cx, 4, he01), (5, None));
    assert_eq!(misc::system_nickname(&mut cx, 64, unknown), (0, None));
    assert!(misc::nav_map_scale(&mut cx, he01).is_ok());
    assert_eq!(misc::nav_map_scale(&mut cx, unknown), Err(PubError::FAILED));
    assert_eq!(
        misc::translate_system_to_msg_id_prefix(&mut cx, he01),
        Ok(b"gcs_refer_system_he01".to_vec())
    );
    assert_eq!(
        misc::translate_system_to_msg_id_prefix(&mut cx, unknown),
        Err(PubError::BAD_ARGUMENT)
    );
    assert!(!misc::single_player(&cx));
    let mut reader = misc::build_system_reader(&mut cx, &source, he01).expect("he01's file");
    assert!(reader.read_header());
    assert!(misc::build_system_reader(&mut cx, &source, unknown).is_err());

    // The system's name, and a miss reported as Common does.
    assert_eq!(system::name(&mut cx, he01), Ok(196_609));
    let before = cx.logic.reports.0.len();
    assert_eq!(system::name(&mut cx, unknown), Err(PubError::BAD_ID));
    assert_eq!(
        cx.logic.reports.0[before].text,
        format!("*** WARNING: get_system(0X{unknown:X}) failed.")
    );
    assert_eq!(system::name(&mut cx, 0), Err(PubError::BAD_ID));
    assert_eq!(cx.logic.reports.0.len(), before + 1, "id 0: no report");

    // Zones by position: the zone tree's answers that the cull keeps.
    let sphere = Sphere {
        center: centre,
        radius: 0.0,
    };
    let expected: u32 = cx
        .world
        .data()
        .universe
        .system_zones(he01)
        .filter(|z| !z.sphere_cull(&sphere))
        .fold(0, |acc, z| acc | z.property_flags);
    assert_eq!(
        system::nested_properties(&mut cx, he01, centre),
        Ok(expected)
    );
    assert_eq!(
        system::nested_properties(&mut cx, unknown, centre),
        Err(PubError::BAD_ID)
    );
    let mut found = system::in_zones(&mut cx, he01, centre, 0.0, 100).unwrap();
    let mut all: Vec<u32> = cx
        .world
        .data()
        .universe
        .system_zones(he01)
        .filter(|z| !z.sphere_cull(&sphere))
        .map(|z| z.nickname)
        .collect();
    for zone in [death, burn, planet_zone] {
        assert!(found.contains(&zone));
    }
    found.sort_unstable();
    all.sort_unstable();
    assert_eq!(found, all);
    assert_eq!(
        system::in_zones(&mut cx, he01, centre, 0.0, 1)
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        system::in_zones(&mut cx, he01, centre, -1.0, 10),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        system::in_zones(&mut cx, unknown, centre, 1.0, 10),
        Err(PubError::BAD_ID)
    );
    // A frustum 20 km up the Z axis looking down it sees the death zone;
    // turned round it does not.
    let apex = Vector::new(centre.x, 0.0, centre.z + 20000.0);
    let looking = Transform::new(Matrix::IDENTITY, apex);
    let seen = system::in_zones_frustum(&mut cx, he01, looking, 1.0, 1.0, 30000.0, 100).unwrap();
    assert!(seen.contains(&death), "{seen:?}");
    let back = Matrix::from_rows([[-1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, -1.0]]);
    let away = Transform::new(back, apex);
    let unseen = system::in_zones_frustum(&mut cx, he01, away, 1.0, 1.0, 10000.0, 100).unwrap();
    assert!(!unseen.contains(&death), "{unseen:?}");

    // Connections: every one, then only jump gates; another type none.
    let all = system::enumerate_connections(&mut cx, he01, 0).unwrap();
    assert!(!all.is_empty());
    assert!(all.iter().any(|c| c.goto_system == he02));
    let gates = system::enumerate_connections(&mut cx, he01, 1).unwrap();
    let holes = system::enumerate_connections(&mut cx, he01, 2).unwrap();
    assert!(gates.len() + holes.len() <= all.len());
    assert!(
        system::enumerate_connections(&mut cx, he01, 3)
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        system::enumerate_connections(&mut cx, unknown, 0),
        Err(PubError::BAD_ID)
    );
    let gate = all.iter().find(|c| c.goto_system == he02).unwrap();
    assert_eq!(misc::system_gate_connection(&cx, gate.object_id), Ok(he02));
    assert_eq!(
        misc::system_gate_connection(&cx, 12345),
        Err(PubError::BAD_ID)
    );

    // The system file's objects, and an enumerator that stops.
    let mut objects = Vec::new();
    assert_eq!(
        system::enumerate_objects(&mut cx, &source, he01, |o| {
            objects.push(o.clone());
            true
        }),
        Ok(())
    );
    assert_eq!(objects.len(), 140);
    assert!(objects.iter().all(|o| o.system == he01));
    let planet = objects.iter().find(|o| o.nickname == b"he01_01").unwrap();
    assert_eq!(planet.pos, centre);
    assert_eq!(planet.ids_name, 196_766);
    assert_eq!(planet.reputation, b"he_p_grp");
    assert_eq!(planet.archetype, create_id(b"planet_earthgrncld_4000"));
    let mut calls = 0;
    system::enumerate_objects(&mut cx, &source, he01, |_| {
        calls += 1;
        calls < 5
    })
    .unwrap();
    assert_eq!(calls, 5);
    assert_eq!(
        system::enumerate_objects(&mut cx, &source, unknown, |_| true),
        Err(PubError::BAD_ID)
    );

    // The zones in file order, the reader at each one's header.
    let mut zones = Vec::new();
    system::enumerate_zones(&mut cx, &source, he01, |id, reader| {
        assert!(reader.read_value());
        let named = create_id(&reader.value_string());
        zones.push((id, named));
    })
    .unwrap();
    let listed = cx.world.data().universe.systems[&he01].zones.clone();
    assert_eq!(zones.len(), listed.len());
    assert!(zones.iter().map(|z| z.0).eq(listed.iter().copied()));
    assert!(zones.iter().all(|(id, named)| id == named));
    assert_eq!(
        system::enumerate_zones(&mut cx, &source, unknown, |_, _| {}),
        Err(PubError::BAD_ID)
    );

    // Solars by nickname once the system loads.
    assert_eq!(
        system::find(&mut cx, he01, b"he01_01"),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        system::find(&mut cx, he01, b""),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        system::find(&mut cx, 0, b"he01_01"),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        system::find(&mut cx, unknown, b"he01_01"),
        Err(PubError::BAD_ID)
    );
    assert_eq!(system::load_system(&mut cx, unknown), Err(PubError::FAILED));
    assert_eq!(system::load_system(&mut cx, he01), Ok(()));
    assert!(cx.logic.server_world.system(he01).unwrap().loaded);
    assert!(
        cx.logic
            .server_world
            .system(he01)
            .unwrap()
            .observers
            .is_empty()
    );
    let planet_id = system::find(&mut cx, he01, b"he01_01").expect("the planet");
    let planet_h = cx.world.find(planet_id).unwrap();
    assert_eq!(cx.world.get(planet_h).unwrap().engine.position, centre);
    let sun_id = system::find(&mut cx, he01, b"Sun_he01").expect("the sun");

    // Objects in a sphere: the solar BSP for a positive radius, the solar
    // list for 0 (exact positions only), ships first.
    let near = system::scan_objects(&mut cx, he01, 100, centre, 1000.0, 0xffff).unwrap();
    assert!(
        near.contains(&planet_id) && !near.contains(&sun_id),
        "{near:?}"
    );
    let exact = system::scan_objects(&mut cx, he01, 100, centre, 0.0, 0xffff).unwrap();
    assert!(exact.contains(&planet_id) && !exact.contains(&sun_id));
    let every = system::scan_objects(&mut cx, he01, 1000, centre, -1.0, 0xffff_ffff).unwrap();
    assert_eq!(
        every.len(),
        cx.world
            .objects_in(he01)
            .into_iter()
            .filter(|&h| cx
                .world
                .get(h)
                .is_some_and(|o| o.kind == game_sim::object::ObjectKind::Solar))
            .count()
    );
    assert_eq!(
        system::scan_objects(&mut cx, he01, 2, centre, -1.0, 0xffff)
            .unwrap()
            .len(),
        2
    );
    assert!(
        system::scan_objects(&mut cx, he01, 0, centre, 1.0e6, 0xffff)
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        system::scan_objects(&mut cx, unknown, 10, centre, 1.0, 0xffff),
        Err(PubError::BAD_ID)
    );
    let ship = spawn_ship(&mut cx, he01, centre);
    let ship_id = cx.world.get(ship).unwrap().simple.id;
    let ships = system::scan_objects(&mut cx, he01, 100, centre, 6000.0, 0x7f_ffff).unwrap();
    assert_eq!(ships.first(), Some(&ship_id), "ships first: {ships:?}");
    assert!(ships.contains(&planet_id));
    assert!(
        system::scan_objects(&mut cx, he01, 100, centre, 6000.0, 0x7f_0000)
            .unwrap()
            .eq(&[ship_id])
    );
    // A ship being destroyed is not a ship any more for the scan.
    cx.logic.objects.entry(cx.world, ship).unwrap().destroyed = true;
    assert!(
        system::scan_objects(&mut cx, he01, 100, centre, 6000.0, 0x7f_0000)
            .unwrap()
            .is_empty()
    );
    assert!(
        sink.take()
            .iter()
            .all(|(_, m)| !matches!(m, ServerMessage::SetReputation(_)))
    );
}

fn spawn_ship(cx: &mut Cx<'_>, system: u32, at: Vector) -> ObjHandle {
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(at.x + 5000.0, at.y, at.z),
        Matrix::IDENTITY,
    );
    cx.world
        .spawn_ship(system, &spawn)
        .expect("he_fighter spawns")
}

fn phantoms_controllers_and_audio(data: Arc<GameData>) {
    let mut world = new_world(data, WorldConfig::default());
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, true);
    let he01 = create_id(b"he01");
    {
        let slot = logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01;
    }
    logic.deliver_hook = Some(|cx, _, _, message| match message {
        ControllerMessage::PhantomEnter {
            object,
            phantom,
            system,
        } => {
            HEARD.with(|h| h.borrow_mut().push((*object, phantom.0, *system, true)));
            // Every export answers -5 while the pump delivers.
            let code = phantom::destroy(cx, *phantom).err().map_or(0, |e| e.0);
            BUSY.with(|b| b.borrow_mut().push(code));
        }
        ControllerMessage::PhantomExit {
            object,
            phantom,
            system,
        } => HEARD.with(|h| h.borrow_mut().push((*object, phantom.0, *system, false))),
        _ => {}
    });
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // A controller to hear the broadcasts.
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    world::add_player(&mut cx, he01, 1);
    sink.take();
    let at = Vector::new(20000.0, 3000.0, -25000.0);

    // Argument checks.
    assert_eq!(
        phantom::create_sphere(&mut cx, he01, 0.0, at, 0xffff_ffff),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        phantom::create_sphere(&mut cx, he01, f32::NAN, at, 0xffff_ffff),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        phantom::create_sphere(&mut cx, 0x4000_0000, 10.0, at, 0xffff_ffff),
        Err(PubError::BAD_ARGUMENT)
    );
    let flat = Vector::new(10.0, 0.0, 10.0);
    assert_eq!(
        phantom::create_box(&mut cx, he01, flat, at, Matrix::IDENTITY, 0xffff_ffff),
        Err(PubError::BAD_ARGUMENT)
    );

    // A ball round where a ship will be, and a box elsewhere.
    let ball = phantom::create_sphere(&mut cx, he01, 500.0, at, 0xffff_ffff).unwrap();
    let size = Vector::new(100.0, 100.0, 100.0);
    let far = Vector::new(-20000.0, 3000.0, 25000.0);
    let cube =
        phantom::create_box(&mut cx, he01, size, far, Matrix::IDENTITY, 0xffff_ffff).unwrap();
    assert_eq!((ball, cube), (PhantomKey(1), PhantomKey(2)));
    assert_eq!(phantom::set_active(&mut cx, cube, false), Ok(()));
    assert_eq!(
        phantom::set_active(&mut cx, PhantomKey(9), true),
        Err(PubError::BAD_ID)
    );

    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    let ship = cx
        .world
        .spawn_ship(he01, &spawn)
        .expect("he_fighter spawns");
    let ship_id = cx.world.get(ship).unwrap().simple.id;
    for _ in 0..3 {
        world::physics_update(&mut cx, 0.1);
    }
    let heard = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    assert_eq!(
        heard,
        [(ship_id, ball.0, he01, true)],
        "the ship entered the ball"
    );
    assert_eq!(BUSY.with(|b| b.borrow().clone()), [PubError::BUSY.0]);
    assert!(!cx.logic.phantoms.pump_busy);
    assert!(cx.logic.phantoms.list.iter().all(|p| p.entered.is_empty()));

    // Attaching and detaching.
    assert_eq!(phantom::attach(&mut cx, 12345, cube), Err(PubError::BAD_ID));
    assert_eq!(
        phantom::attach(&mut cx, ship_id, PhantomKey(9)),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(phantom::attach(&mut cx, ship_id, cube), Ok(()));
    assert_eq!(
        phantom::attach(&mut cx, ship_id, cube),
        Err(PubError::FAILED)
    );
    assert_eq!(phantom::detach(&mut cx, cube), Ok(()));
    assert_eq!(
        phantom::detach(&mut cx, PhantomKey(9)),
        Err(PubError::BAD_ID)
    );

    // Switched off, the ball loses its intruder: `PhantomExit`.
    assert_eq!(phantom::set_active(&mut cx, ball, false), Ok(()));
    world::physics_update(&mut cx, 0.1);
    let heard = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    assert_eq!(heard, [(ship_id, ball.0, he01, false)], "the ship left");
    assert!(cx.logic.phantoms.list.iter().all(|p| p.left.is_empty()));

    // Destroyed, a phantom sends no leave events and its record goes.
    assert_eq!(phantom::set_active(&mut cx, ball, true), Ok(()));
    assert_eq!(phantom::destroy(&mut cx, ball), Ok(()));
    assert_eq!(phantom::destroy(&mut cx, ball), Err(PubError::BAD_ID));
    world::physics_update(&mut cx, 0.1);
    assert!(HEARD.with(|h| h.borrow().is_empty()));
    cx.logic.phantoms.pump_busy = true;
    assert_eq!(
        phantom::create_sphere(&mut cx, he01, 10.0, at, 1),
        Err(PubError::BUSY)
    );
    assert_eq!(phantom::detach(&mut cx, cube), Err(PubError::BUSY));
    cx.logic.phantoms.pump_busy = false;
    assert_eq!(phantom::destroy(&mut cx, cube), Ok(()));
    assert!(cx.logic.phantoms.list.is_empty());

    // The controller bus.
    let id = controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Normal,
    );
    assert_ne!(id, ControllerId::BROADCAST);
    let unknown = controller::create(
        &mut cx,
        b"Content.dll",
        b"NoSuchKind",
        &CreateParams::None,
        Priority::Normal,
    );
    assert_eq!(unknown, ControllerId::BROADCAST);
    assert_eq!(controller::set_heartbeat_interval(&mut cx, id, 2.0), Ok(()));
    assert_eq!(cx.logic.bus.find(id).unwrap().heartbeat_period, 2.0);
    assert_eq!(
        controller::set_heartbeat_interval(&mut cx, ControllerId(0x1234_5678), 2.0),
        Err(PubError::BAD_ID)
    );
    let mut message = ControllerMessage::SystemLoaded { system: he01 };
    assert_eq!(controller::send_message(&mut cx, id, &mut message), Ok(()));
    assert_eq!(
        controller::send_message(&mut cx, ControllerId::BROADCAST, &mut message),
        Ok(())
    );
    assert_eq!(
        controller::send_message(&mut cx, ControllerId(0x1234_5678), &mut message),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        controller::set_timer(&mut cx, id, 1, 2, -1.0),
        Err(PubError::BAD_ARGUMENT)
    );
    let timers = cx.logic.bus.timers().len();
    assert_eq!(
        controller::set_timer(&mut cx, ControllerId(77), 1, 2, 0.5),
        Ok(())
    );
    assert_eq!(
        cx.logic.bus.timers().len(),
        timers + 1,
        "the id is not checked"
    );
    controller::destroy(&mut cx, id);
    assert!(cx.logic.bus.destroy_pending().any(|p| p == id));

    // Audio: one packet each, always 0.
    sink.take();
    let track = audio::Tryptich {
        space: 1,
        danger: 2,
        battle: 3,
        override_music: 4,
        flag: 5,
        fade: 0.25,
    };
    assert_eq!(audio::set_music(&mut cx, 1, &track), 0);
    assert_eq!(audio::cancel_music(&mut cx, 1), 0);
    assert_eq!(audio::play_sound_effect(&mut cx, 3, 99), 0);
    match &sink.take()[..] {
        [
            (1, ServerMessage::SetMusic(m)),
            (1, ServerMessage::CancelMusic(_)),
            (3, ServerMessage::PlaySoundEffect(s)),
        ] => {
            assert_eq!(
                (
                    m.field_20, m.field_24, m.field_28, m.field_2c, m.field_30, m.field_34
                ),
                (1, 2, 3, 4, 1, 0.25)
            );
            assert_eq!(s.value, 99);
        }
        other => panic!("{other:?}"),
    }
}
