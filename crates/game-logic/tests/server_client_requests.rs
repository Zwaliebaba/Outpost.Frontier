//! The client's flight and combat requests on the server (work package
//! P3.10a) over the original data (`outdir/`), without a network: each
//! decoded message goes through [`receive::deliver`] as the host hands it
//! on, changes the world as `IServerImpl`'s handler does
//! (`Server/IServerImpl.cpp:412-575`, `1694-2027`, `2756`) and tells the
//! other players that can see the object, never the sender. The refusals
//! (a destroyed object, another player's ship, a session without a ship,
//! a reason the server does not grant) change nothing.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::receive;
use game_logic::replication;
use game_logic::server::{ClientSink, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipKind, SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT, SubObjId, types};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, ObjectKind, World};
use game_sim::protocol::common::{
    ActivateCruise, ActivateEquip, ActivateThrusters, CommonMessage, FireWeapon, GoTradeLane,
    JettisonCargo, SetTarget, StopTradeLane, TractorObjects,
};
use game_sim::protocol::to_client::ServerMessage;
use game_sim::protocol::to_server::{
    BadlandsObjCollision, Dock, MineAsteroid, MunCollision, ObjCollision, ReqInvincibility,
    RequestUseItem, SetManeuver,
};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

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

fn setup(what: &str, single_player: bool, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    let world = World::new(Arc::new(data), config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, !single_player);
    logic.deliver_hook = Some(|_, _, _, _| {});
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

fn fighter(cx: &mut Cx<'_>, owner: u32, at: Vector) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    spawn.owner_player = owner;
    let h = cx
        .world
        .spawn_ship(he01(), &spawn)
        .expect("he_fighter spawns");
    // The server's wrappers of its equipment (`EqObj::rebuild_server_equip`).
    game_logic::ship::rebuild_server_equip(cx, h);
    h
}

/// A cargo stack added to the ship's equipment (no player record).
fn stack(cx: &mut Cx<'_>, h: ObjHandle, arch: &[u8], count: i32) -> SubObjId {
    let id = game_logic::ship::allocate_equip_id(cx, h);
    let mut desc = EquipDesc::new(cx.world.strings());
    desc.id = id;
    desc.arch_id = create_id(arch);
    desc.count = count;
    desc.health = 1.0;
    desc.make_internal(cx.world.strings());
    cx.world.add_equip_item(h, &desc);
    id
}

/// The record of `h` in the observer of `player` is created on its client.
fn created_for(cx: &mut Cx<'_>, player: u32, h: ObjHandle) {
    let world: &World = cx.world;
    let v = cx
        .logic
        .server_world
        .systems
        .get_mut(&he01())
        .unwrap()
        .observers
        .iter_mut()
        .find(|o| o.player == player)
        .expect("the observer");
    let at = match v.tracked.iter().position(|t| t.object == h) {
        Some(at) => at,
        None => replication::add_tracked_object(world, v, h),
    };
    v.tracked[at].created_on_client = true;
}

/// He01 with players 1 to 3: player 1 flies `ship` (its own), `npc` is an
/// NPC fighter beside it; every client has both.
struct Scene {
    ship: ObjHandle,
    npc: ObjHandle,
}

fn scene(cx: &mut Cx<'_>) -> Scene {
    for session in 1..=3 {
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = he01();
    }
    cx.world.load_system(he01());
    let ship = fighter(cx, 1, Vector::new(15000.0, 3000.0, -25000.0));
    let npc = fighter(cx, 0, Vector::new(15500.0, 3000.0, -25000.0));
    let ship_id = id(cx, ship);
    let slot = cx.logic.players.get_mut(1).unwrap();
    slot.ship_object_id = ship_id;
    slot.relative_health = 1.0;
    for session in 1..=3 {
        world::add_player(cx, he01(), session);
    }
    for session in 1..=3 {
        created_for(cx, session, ship);
        created_for(cx, session, npc);
    }
    Scene { ship, npc }
}

fn common(sent: &[(u32, ToClient)]) -> Vec<(u32, CommonMessage)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(x) => Some((*c, x.clone())),
            ToClient::Server(_) => None,
        })
        .collect()
}

fn server(sent: &[(u32, ToClient)]) -> Vec<(u32, ServerMessage)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Server(x) => Some((*c, x.clone())),
            ToClient::Common(_) => None,
        })
        .collect()
}

fn deliver_common(cx: &mut Cx<'_>, session: u32, message: impl Into<CommonMessage>) {
    assert!(receive::deliver(
        cx,
        session,
        &ToServer::Common(message.into())
    ));
}

fn deliver_client(cx: &mut Cx<'_>, session: u32, message: impl Into<ClientMessage>) {
    assert!(receive::deliver(
        cx,
        session,
        &ToServer::Client(message.into())
    ));
}

fn items(cx: &Cx<'_>, h: ObjHandle, mask: u32) -> Vec<SubObjId> {
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    eq.equipment
        .traverse(mask, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot).map(|e| e.id))
        .collect()
}

fn cruising(cx: &Cx<'_>, h: ObjHandle) -> bool {
    replication::cruise_active(cx.world, h)
}

fn thrusters_on(cx: &Cx<'_>, h: ObjHandle) -> Vec<bool> {
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    eq.equipment
        .traverse(types::THRUSTER, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot).map(|e| e.is_active()))
        .collect()
}

fn destroy(cx: &mut Cx<'_>, h: ObjHandle) {
    cx.logic.objects.entry(cx.world, h).unwrap().destroyed = true;
}

#[test]
fn fire_cruise_thrusters_and_equipment_reach_the_other_players() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's flight requests", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, .. } = scene(&mut cx);
    let ship_id = id(&cx, ship);
    let _ = sink.take();

    // A shot of the guns: the server pays and the others hear of it.
    let guns = items(&cx, ship, types::GUN);
    assert!(!guns.is_empty(), "the fighter has guns");
    let target = FireWeapon::target_from([15000.0, 3000.0, -24000.0]);
    let fire = FireWeapon {
        object_id: ship_id,
        target,
        hardpoints: guns.clone(),
    };
    deliver_common(&mut cx, 1, fire.clone());
    let got = common(&sink.take());
    let fired: Vec<(u32, FireWeapon)> = got
        .into_iter()
        .filter_map(|(c, m)| match m {
            CommonMessage::FireWeapon(f) => Some((c, f)),
            _ => None,
        })
        .collect();
    assert_eq!(fired.iter().map(|(c, _)| *c).collect::<Vec<_>>(), [2, 3]);
    assert_eq!(fired[0].1.object_id, ship_id);
    assert!(!fired[0].1.hardpoints.is_empty());

    // Cruise: the engine engages on the multiplayer server.
    assert!(!cruising(&cx, ship));
    deliver_common(
        &mut cx,
        1,
        ActivateCruise {
            object_id: ship_id,
            active: 1,
            field_05: 0,
        },
    );
    assert!(cruising(&cx, ship));
    let expected = CommonMessage::ActivateCruise(ActivateCruise {
        object_id: ship_id,
        active: 1,
        field_05: 0,
    });
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);
    deliver_common(
        &mut cx,
        1,
        ActivateCruise {
            object_id: ship_id,
            active: 0,
            field_05: 0,
        },
    );
    assert!(!cruising(&cx, ship));
    let _ = sink.take();

    // Thrusters: switched on, and the others hear of it.
    deliver_common(
        &mut cx,
        1,
        ActivateThrusters {
            object_id: ship_id,
            active: 1,
        },
    );
    assert!(thrusters_on(&cx, ship).iter().all(|&on| on));
    let expected = CommonMessage::ActivateThrusters(ActivateThrusters {
        object_id: ship_id,
        active: 1,
    });
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);

    // One item: the first thruster off.
    if let Some(&thruster) = items(&cx, ship, types::THRUSTER).first() {
        deliver_common(
            &mut cx,
            1,
            ActivateEquip {
                object_id: ship_id,
                equip_id: thruster,
                active: 0,
            },
        );
        assert!(!thrusters_on(&cx, ship)[0]);
        let expected = CommonMessage::ActivateEquip(ActivateEquip {
            object_id: ship_id,
            equip_id: thruster,
            active: 0,
        });
        assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);
    }

    // A destroyed ship's requests do nothing.
    destroy(&mut cx, ship);
    deliver_common(&mut cx, 1, fire);
    deliver_common(
        &mut cx,
        1,
        ActivateCruise {
            object_id: ship_id,
            active: 1,
            field_05: 0,
        },
    );
    assert!(!cruising(&cx, ship));
    assert!(sink.take().is_empty());
}

#[test]
fn a_single_player_request_is_relayed_without_changing_the_ship() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("single player's relay", true, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, .. } = scene(&mut cx);
    let ship_id = id(&cx, ship);
    let _ = sink.take();
    deliver_common(
        &mut cx,
        1,
        ActivateCruise {
            object_id: ship_id,
            active: 1,
            field_05: 1,
        },
    );
    // The client's own world engaged it; the server's is the same world
    // in single player and is left as it is.
    assert!(!cruising(&cx, ship));
    let expected = CommonMessage::ActivateCruise(ActivateCruise {
        object_id: ship_id,
        active: 1,
        field_05: 1,
    });
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);
}

#[test]
fn targets_of_the_ship_and_of_a_gun() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's targets", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, npc } = scene(&mut cx);
    let (ship_id, npc_id) = (id(&cx, ship), id(&cx, npc));
    let _ = sink.take();

    // The ship's own target.
    let target = SetTarget {
        object_id: ship_id,
        target_id: npc_id,
        sub_obj_id: SUBOBJ_ID_NONE,
        target_sub_obj_id: SUBOBJ_ID_ROOT,
    };
    deliver_common(&mut cx, 1, target.clone());
    let s = cx.world.get(ship).unwrap().as_ship().unwrap();
    assert_eq!((s.target, s.sub_target), (Some(npc), SUBOBJ_ID_ROOT));
    let expected = CommonMessage::SetTarget(target);
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);

    // A gun's target.
    let gun = items(&cx, ship, types::GUN)[0];
    let gun_target = SetTarget {
        object_id: ship_id,
        target_id: npc_id,
        sub_obj_id: gun,
        target_sub_obj_id: SUBOBJ_ID_ROOT,
    };
    deliver_common(&mut cx, 1, gun_target.clone());
    assert_eq!(
        cx.world.gun_target(ship, gun),
        Some((Some(npc), SUBOBJ_ID_ROOT))
    );
    let expected = CommonMessage::SetTarget(gun_target);
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);

    // An item that is not a gun is refused (-4), and a client's request is
    // told all the same.
    let thruster = items(&cx, ship, types::THRUSTER)[0];
    deliver_common(
        &mut cx,
        1,
        SetTarget {
            object_id: ship_id,
            target_id: npc_id,
            sub_obj_id: thruster,
            target_sub_obj_id: SUBOBJ_ID_ROOT,
        },
    );
    assert_eq!(common(&sink.take()).len(), 2);
}

#[test]
fn the_lanes_take_only_the_players_own_ship() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's lanes", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, npc } = scene(&mut cx);
    let (ship_id, npc_id) = (id(&cx, ship), id(&cx, npc));
    let (ring16, ring17) = (
        create_id(b"he01_Trade_Lane_Ring_16"),
        create_id(b"he01_Trade_Lane_Ring_17"),
    );
    let _ = sink.take();

    // Another ship than the session's: refused.
    deliver_common(
        &mut cx,
        1,
        GoTradeLane {
            object_id: npc_id,
            tradelane_ring1: ring16,
            tradelane_ring2: ring17,
            field_0c: 0,
        },
    );
    assert!(sink.take().is_empty());
    assert!(
        !cx.world
            .get(npc)
            .unwrap()
            .as_ship()
            .unwrap()
            .using_tradelane
    );

    let go = GoTradeLane {
        object_id: ship_id,
        tradelane_ring1: ring16,
        tradelane_ring2: ring17,
        field_0c: 1,
    };
    deliver_common(&mut cx, 1, go.clone());
    assert!(
        cx.world
            .get(ship)
            .unwrap()
            .as_ship()
            .unwrap()
            .using_tradelane
    );
    let expected = CommonMessage::GoTradeLane(go);
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);

    let stop = StopTradeLane {
        ship_id,
        tradelane_ring1: ring16,
        tradelane_ring2: ring17,
    };
    deliver_common(&mut cx, 2, stop.clone());
    assert!(sink.take().is_empty(), "player 2 does not fly the ship");
    deliver_common(&mut cx, 1, stop.clone());
    assert!(
        !cx.world
            .get(ship)
            .unwrap()
            .as_ship()
            .unwrap()
            .using_tradelane
    );
    let expected = CommonMessage::StopTradeLane(stop);
    assert_eq!(common(&sink.take()), [(2, expected.clone()), (3, expected)]);
}

#[test]
fn cargo_jettisoned_and_used() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's cargo", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, .. } = scene(&mut cx);
    let ship_id = id(&cx, ship);
    let kits = stack(&mut cx, ship, b"ge_s_repair_01", 10);
    let count = |cx: &Cx<'_>| {
        cx.world
            .get(ship)
            .unwrap()
            .equipped
            .as_deref()
            .unwrap()
            .equipment
            .by_id(kits)
            .map(|e| match &e.kind {
                EquipKind::Cargo(c) => c.count,
                _ => 0,
            })
    };
    let _ = sink.take();

    // A whole hull takes no kit: refused, nothing told.
    deliver_client(
        &mut cx,
        1,
        RequestUseItem {
            object_id: ship_id,
            item_id: kits,
            count: 3,
        },
    );
    assert_eq!(count(&cx), Some(10));
    assert!(sink.take().is_empty());

    // A hull at half health takes what it needs, at most the three asked.
    let max = cx.world.get(ship).unwrap().simple.max_hit_pts;
    cx.world.get_mut(ship).unwrap().simple.hit_pts = max * 0.5;
    deliver_client(
        &mut cx,
        1,
        RequestUseItem {
            object_id: ship_id,
            item_id: kits,
            count: 3,
        },
    );
    let per_kit = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&create_id(b"ge_s_repair_01"))
        .unwrap()
        .root
        .hit_pts;
    let needed = ((f64::from(max) - f64::from(max * 0.5)) / f64::from(per_kit)).ceil() as u32;
    let used = needed.min(3);
    assert_eq!(count(&cx), Some(10 - used));
    let hull = cx.world.get(ship).unwrap().simple.hit_pts;
    assert!(hull > max * 0.5, "{hull} of {max}");
    let got = server(&sink.take());
    let uses: Vec<u32> = got
        .iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::UseItem(u) if u.object_id == ship_id && u.equip_id == kits => {
                assert_eq!(u32::from(u.count), used);
                Some(*c)
            }
            _ => None,
        })
        .collect();
    assert_eq!(uses, [1, 2, 3], "the owner hears of it too");

    // The jettison of two kits: the others and the owner hear of it.
    deliver_common(
        &mut cx,
        1,
        JettisonCargo {
            object_id: ship_id,
            item_id: kits,
            count: 2,
        },
    );
    assert_eq!(count(&cx), Some(10 - used - 2));
    let got = common(&sink.take());
    let jettisons: Vec<u32> = got
        .iter()
        .filter_map(|(c, m)| matches!(m, CommonMessage::JettisonCargo(_)).then_some(*c))
        .collect();
    assert_eq!(jettisons, [1, 2, 3]);
}

#[test]
fn collisions_reported_by_a_client() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's collisions", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, npc } = scene(&mut cx);
    let (ship_id, npc_id) = (id(&cx, ship), id(&cx, npc));
    let gun = items(&cx, ship, types::GUN)[0];
    let munition = cx
        .world
        .launcher_projectile(ship, gun)
        .expect("a projectile")
        .nickname;
    let health = |cx: &Cx<'_>, h: ObjHandle| {
        let o = cx.world.get(h).unwrap();
        let shield: f32 = o
            .equipped
            .as_deref()
            .unwrap()
            .equipment
            .traverse(types::SHIELD, false)
            .into_iter()
            .filter_map(|s| o.equipped.as_deref().unwrap().equipment.get(s))
            .map(|e| e.hit_points())
            .sum();
        (o.simple.hit_pts, shield)
    };
    let hit = MunCollision {
        field_00: munition,
        field_04: ship_id,
        object_id: npc_id,
        field_0c: SUBOBJ_ID_ROOT,
        hit_position: [15500.0, 3000.0, -25000.0],
    };

    // A session without a ship reports nothing.
    let before = health(&cx, npc);
    deliver_client(&mut cx, 2, hit.clone());
    assert_eq!(health(&cx, npc), before);
    // Nor does one whose ship is (nearly) dead.
    cx.logic.players.get_mut(1).unwrap().relative_health = 0.0001;
    deliver_client(&mut cx, 1, hit.clone());
    assert_eq!(health(&cx, npc), before);
    cx.logic.players.get_mut(1).unwrap().relative_health = 1.0;
    deliver_client(&mut cx, 1, hit);
    let after = health(&cx, npc);
    assert!(
        after.0 < before.0 || after.1 < before.1,
        "{before:?} {after:?}"
    );

    // An object collision: the other object deals the damage.
    let before = after;
    deliver_client(
        &mut cx,
        1,
        ObjCollision {
            object_id: npc_id,
            sub_obj_id: SUBOBJ_ID_ROOT,
            target_object_id: ship_id,
            target_sub_obj_id: SUBOBJ_ID_ROOT,
            field_10: 50.0,
        },
    );
    let after = health(&cx, npc);
    assert!(
        after.0 < before.0 || after.1 < before.1,
        "{before:?} {after:?}"
    );

    // The badlands: an invincible ship stays, any other is destroyed.
    let badlands = BadlandsObjCollision {
        object_id: npc_id,
        field_08: 0,
        field_00: ship_id,
    };
    cx.logic.objects.entry(cx.world, npc).unwrap().invincible = true;
    deliver_client(&mut cx, 1, badlands.clone());
    assert!(!cx.logic.objects.get(npc).unwrap().destroyed);
    cx.logic.objects.entry(cx.world, npc).unwrap().invincible = false;
    deliver_client(&mut cx, 1, badlands);
    assert!(
        cx.logic.objects.get(npc).is_none_or(|o| o.destroyed)
            || cx.world.get(npc).is_none_or(|o| o.dead)
    );
}

#[test]
fn mined_loot_tractor_invincibility_manoeuvre_and_the_forced_landing() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the client's other requests", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { ship, npc } = scene(&mut cx);
    let (ship_id, npc_id) = (id(&cx, ship), id(&cx, npc));
    let _ = sink.take();

    // Mined loot: one container of the contents, from nobody, mode 3.
    let loot_of = |cx: &Cx<'_>| -> Vec<ObjHandle> {
        cx.world
            .objects_in(he01())
            .into_iter()
            .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Loot))
            .collect()
    };
    let before = loot_of(&cx).len();
    deliver_client(
        &mut cx,
        1,
        MineAsteroid {
            system_id: he01(),
            position: [15200.0, 3000.0, -25000.0],
            loot_id: 0,
            secondary_loot: create_id(b"ge_s_repair_01"),
            quantity: 4,
        },
    );
    let loot = loot_of(&cx);
    assert_eq!(loot.len(), before + 1);
    let container = *loot.last().unwrap();
    match &cx.world.get(container).unwrap().data {
        game_sim::object::KindData::Loot(l) => {
            assert_eq!(l.contents_arch, create_id(b"ge_s_repair_01"));
            assert_eq!(l.units, 4);
        }
        other => panic!("{other:?}"),
    }
    // On a multiplayer server every observer tracks it; the packet follows
    // from its update.
    for session in 1..=3 {
        let tracked = cx.logic.server_world.systems[&he01()]
            .observers
            .iter()
            .find(|o| o.player == session)
            .unwrap()
            .find(container)
            .is_some();
        assert!(tracked, "player {session}");
    }
    // An unknown system makes nothing.
    deliver_client(
        &mut cx,
        1,
        MineAsteroid {
            system_id: create_id(b"no_such_system"),
            position: [0.0; 3],
            loot_id: 0,
            secondary_loot: create_id(b"ge_s_repair_01"),
            quantity: 4,
        },
    );
    assert_eq!(loot_of(&cx).len(), before + 1);
    let _ = sink.take();

    // A tractor without a tractor item: -2, and the others are told all the
    // same (a client's request).
    let container = *loot.last().unwrap();
    let container_id = id(&cx, container);
    let tractor = items(&cx, ship, types::TRACTOR).first().copied();
    deliver_common(
        &mut cx,
        1,
        TractorObjects {
            object_id: ship_id,
            tractor_equip_id: tractor.unwrap_or(0x7777),
            target_ids: vec![container_id, 0x3fff_fff0],
        },
    );
    let got = common(&sink.take());
    let tractors: Vec<(u32, Vec<u32>)> = got
        .iter()
        .filter_map(|(c, m)| match m {
            CommonMessage::TractorObjects(t) => Some((*c, t.target_ids.clone())),
            _ => None,
        })
        .collect();
    assert_eq!(tractors.iter().map(|(c, _)| *c).collect::<Vec<_>>(), [2, 3]);
    // The unknown id is left out before the slot runs.
    assert!(tractors.iter().all(|(_, ids)| ids.len() <= 1));

    // Invincibility: reason 5 grants it, an unknown reason is refused, the
    // ship must be the session's.
    let request = |on: u8, reason: u8, ship: u32| ReqInvincibility {
        ship_id: ship,
        reason,
        invincible: on,
    };
    deliver_client(&mut cx, 1, request(1, 3, ship_id));
    assert!(!cx.logic.objects.get(ship).is_some_and(|o| o.invincible));
    deliver_client(&mut cx, 2, request(1, 5, ship_id));
    assert!(!cx.logic.objects.get(ship).is_some_and(|o| o.invincible));
    deliver_client(&mut cx, 1, request(1, 5, ship_id));
    let o = cx.logic.objects.get(ship).unwrap();
    assert!(o.invincible && o.invincible_flag);
    assert_eq!(o.invincible_timer, 0.0);
    // Reason 2 needs the ship in a dock queue of the system.
    deliver_client(&mut cx, 1, request(0, 5, ship_id));
    assert!(!cx.logic.objects.get(ship).unwrap().invincible);
    deliver_client(&mut cx, 1, request(1, 2, ship_id));
    assert!(!cx.logic.objects.get(ship).unwrap().invincible);
    let solar = game_logic::dock::collect_dockable_solars(&cx, he01(), 0)[0];
    game_logic::dock::queue_dock_request(&mut cx, solar, 0, ship_id);
    deliver_client(&mut cx, 1, request(1, 2, ship_id));
    assert!(cx.logic.objects.get(ship).unwrap().invincible);

    // A manoeuvre ends a directive waiting for that behaviour (the manager's
    // type-2 event); another behaviour's leaves it.
    cx.world.with_behavior_manager(npc, |m, _| {
        m.state.behavior = 0;
        m.state.directive = [2, 0, 3];
        m.state.f118 = 7;
    });
    let maneuver = |behavior: u32| SetManeuver {
        field_00: ship_id,
        object_id: npc_id,
        field_08: behavior,
    };
    deliver_client(&mut cx, 1, maneuver(4));
    let state = &cx.world.behavior_manager(npc).unwrap().state;
    assert_eq!((state.directive, state.f118), ([2, 0, 3], 7));
    deliver_client(&mut cx, 1, maneuver(3));
    let state = &cx.world.behavior_manager(npc).unwrap().state;
    assert_eq!(
        (state.directive, state.f118, state.f11c),
        ([0, 0, -1], 0, 3)
    );

    // The forced landing at the base of another system: the slot moves.
    let base = create_id(b"he02_01_Base");
    if cx.world.data().universe.bases.contains_key(&base) {
        deliver_client(&mut cx, 1, Dock { value: base });
        assert_eq!(
            cx.logic.players.get(1).unwrap().system_id,
            cx.world.data().universe.bases[&base].system
        );
    }
}
