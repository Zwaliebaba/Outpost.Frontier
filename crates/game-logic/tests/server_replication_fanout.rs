//! Replication's per-object fan-outs over the original data (`outdir/`,
//! record 0007 point 7): the `IObjAffect` notices, the fuses, the comm,
//! the trade lanes, the group changes and the cargo reach the players of
//! the system that can see the object (for a tracked object: whose client
//! has it) but the player that asked; the left-system latch, the landing,
//! a client's own object updates with the remote correction, and the
//! changed affiliations. The expected packets follow the C++
//! (`Server/server_classes.cpp`, `server_observer.cpp`, `IServerImpl.cpp`,
//! `server_gameobject.cpp`).

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::replication::{self, Comm, ObjectUpdate, Observer};
use game_logic::server::{ClientSink, Cx, Logic};
use game_logic::world::Hooks;
use game_logic::{dock, fuses, group, munitions, world};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::costume::Costume;
use game_sim::equip::{SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, ProjectileSpawn, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, ObjectKind, World};
use game_sim::protocol::common::{
    ActivateCruise, ActivateEquip, ActivateThrusters, CommonMessage, FireComponent, FireWeapon,
    GoTradeLane, JettisonCargo, SetTarget, StopTradeLane,
};
use game_sim::protocol::costume::Costume as WireCostume;
use game_sim::protocol::to_client::{BurnFuse, ServerMessage};
use game_sim::protocol::{ToClient, quantise_orientation};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Quaternion, Vector};
use neuron_core::vfs::Host;

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

fn set_ship(cx: &mut Cx<'_>, session: u32, ship: ObjHandle) {
    let ship_id = id(cx, ship);
    cx.logic.players.get_mut(session).unwrap().ship_object_id = ship_id;
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

fn view_mut<'c>(cx: &'c mut Cx<'_>, system: u32, player: u32) -> &'c mut Observer {
    cx.logic
        .server_world
        .systems
        .get_mut(&system)
        .unwrap()
        .observers
        .iter_mut()
        .find(|o| o.player == player)
        .expect("the observer")
}

/// The client of `player` has the object (its record is created).
fn created_for(cx: &mut Cx<'_>, system: u32, player: u32, h: ObjHandle) {
    let v = view_mut(cx, system, player);
    let record = v
        .tracked
        .iter_mut()
        .find(|t| t.object == h)
        .expect("a record");
    record.created_on_client = true;
}

/// The common messages of a kind, with their recipients.
fn common<T>(
    sent: &[(u32, ToClient)],
    pick: impl Fn(&CommonMessage) -> Option<T>,
) -> Vec<(u32, T)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(x) => pick(x).map(|v| (*c, v)),
            ToClient::Server(_) => None,
        })
        .collect()
}

/// The server messages of a kind, with their recipients.
fn server<T>(
    sent: &[(u32, ToClient)],
    pick: impl Fn(&ServerMessage) -> Option<T>,
) -> Vec<(u32, T)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Server(x) => pick(x).map(|v| (*c, v)),
            ToClient::Common(_) => None,
        })
        .collect()
}

fn recipients<T>(list: &[(u32, T)]) -> Vec<u32> {
    list.iter().map(|(c, _)| *c).collect()
}

/// The observer of `player` tracks the object, its client has it.
fn track(cx: &mut Cx<'_>, system: u32, player: u32, h: ObjHandle) {
    let world: &World = cx.world;
    let v = cx
        .logic
        .server_world
        .systems
        .get_mut(&system)
        .unwrap()
        .observers
        .iter_mut()
        .find(|o| o.player == player)
        .expect("the observer");
    let at = replication::add_tracked_object(world, v, h);
    v.tracked[at].created_on_client = true;
}

/// A solar of the system with some of `bits` in its type.
fn solar_with(cx: &Cx<'_>, system: u32, bits: u32) -> ObjHandle {
    cx.world
        .objects_in(system)
        .into_iter()
        .find(|&h| {
            cx.world
                .get(h)
                .is_some_and(|o| o.kind == ObjectKind::Solar && o.simple.type_bits & bits != 0)
        })
        .expect("a solar of the type")
}

/// The switch that changes an object's first docking light.
fn docking_light_switch(cx: &Cx<'_>, h: ObjHandle) -> Option<bool> {
    let data = cx.world.data();
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    eq.equipment
        .traverse(game_sim::equip::types::LIGHT, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot))
        .find(|e| {
            !e.is_destroyed()
                && matches!(
                    data.archetypes.equipment.get(&e.arch_id).map(|a| &a.kind),
                    Some(game_sim::data::archetype::equipment::EquipmentKind::Light {
                        docking_light: true,
                        ..
                    })
                )
        })
        .map(|e| !e.is_active())
}

/// He01 on a multiplayer server with players 1 to 3 and two NPC fighters,
/// `a` and `b`, which every observer tracks; the clients of players 1 and
/// 3 have `a`, player 2's does not.
struct Scene {
    a: ObjHandle,
    b: ObjHandle,
}

fn scene(cx: &mut Cx<'_>) -> Scene {
    for session in 1..=3 {
        join(cx, session, he01());
    }
    cx.world.load_system(he01());
    let a = fighter(cx, he01(), 0, Vector::new(15000.0, 3000.0, -25000.0));
    let b = fighter(cx, he01(), 0, Vector::new(15500.0, 3000.0, -25000.0));
    for session in 1..=3 {
        world::add_player(cx, he01(), session);
    }
    created_for(cx, he01(), 1, a);
    created_for(cx, he01(), 3, a);
    Scene { a, b }
}

#[test]
fn the_affect_notices_reach_the_observers_that_created_the_object_but_the_source() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the affect notices", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, b } = scene(&mut cx);
    let (a_id, b_id) = (id(&cx, a), id(&cx, b));
    let _ = sink.take();

    // `notify_activate_equip`: the asking client (3) is not told, player 2's
    // client does not have the ship.
    replication::equip_activated(&mut cx, he01(), a_id, 0x22, true, 3);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateEquip(x) => Some(x.clone()),
        _ => None,
    });
    let expected = ActivateEquip {
        object_id: a_id,
        equip_id: 0x22,
        active: 1,
    };
    assert_eq!(got, [(1, expected.clone())]);
    // From the server (0) every creator hears of it.
    replication::equip_activated(&mut cx, he01(), a_id, 0x22, true, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateEquip(x) => Some(x.clone()),
        _ => None,
    });
    assert_eq!(got, [(1, expected.clone()), (3, expected)]);

    // `notify_cruise`: the engage byte and the second byte.
    replication::cruise_changed(&mut cx, he01(), a_id, true, false, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateCruise(x) => Some(x.clone()),
        _ => None,
    });
    let cruise = ActivateCruise {
        object_id: a_id,
        active: 1,
        field_05: 0,
    };
    assert_eq!(got, [(1, cruise.clone()), (3, cruise)]);

    // `notify_thrusters`, asked by player 1.
    replication::thrusters_changed(&mut cx, he01(), a_id, false, 1);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateThrusters(x) => Some(x.clone()),
        _ => None,
    });
    let thrusters = ActivateThrusters {
        object_id: a_id,
        active: 0,
    };
    assert_eq!(got, [(3, thrusters)]);

    // `notify_set_target`: the target's id, or 0 without one.
    replication::target_changed(&mut cx, he01(), a_id, SUBOBJ_ID_NONE, Some(b), 7, 0);
    replication::target_changed(&mut cx, he01(), a_id, 0x22, None, SUBOBJ_ID_NONE, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::SetTarget(x) => Some(x.clone()),
        _ => None,
    });
    let first = SetTarget {
        object_id: a_id,
        target_id: b_id,
        sub_obj_id: SUBOBJ_ID_NONE,
        target_sub_obj_id: 7,
    };
    let second = SetTarget {
        object_id: a_id,
        target_id: 0,
        sub_obj_id: 0x22,
        target_sub_obj_id: SUBOBJ_ID_NONE,
    };
    assert_eq!(
        got,
        [
            (1, first.clone()),
            (3, first),
            (1, second.clone()),
            (3, second)
        ]
    );
    // A solar of the system file goes to every player without asking; no
    // object, to nobody.
    let station = solar_with(&cx, he01(), 0x20);
    let station_id = id(&cx, station);
    for session in 1..=3 {
        view_mut(&mut cx, he01(), session).position = Vector::new(1.0e7, 0.0, 0.0);
    }
    replication::target_changed(&mut cx, he01(), station_id, 5, Some(a), 0, 2);
    replication::target_changed(&mut cx, he01(), 0x7fff_fff0, 5, Some(a), 0, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::SetTarget(x) => Some(x.object_id),
        _ => None,
    });
    assert_eq!(got, [(1, station_id), (3, station_id)]);

    // `notify_fire_weapon`: the aim as the multiplayer encoder picks it.
    let aim = Vector::new(15010.0, 3000.0, -24000.0);
    replication::weapon_fired(&mut cx, he01(), a_id, &[0x22, 0x23], aim, 1);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::FireWeapon(x) => Some(x.clone()),
        _ => None,
    });
    let fire = FireWeapon {
        object_id: a_id,
        target: FireWeapon::target_from([aim.x, aim.y, aim.z]),
        hardpoints: vec![0x22, 0x23],
    };
    assert_eq!(got, [(3, fire)]);

    // `notify_jettison`: no player skipped.
    replication::cargo_jettisoned(&mut cx, he01(), a, 0x30, 2);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::JettisonCargo(x) => Some(x.clone()),
        _ => None,
    });
    let jettison = JettisonCargo {
        object_id: a_id,
        item_id: 0x30,
        count: 2,
    };
    assert_eq!(got, [(1, jettison.clone()), (3, jettison)]);

    // `broadcast_fuse`: the record as it was built.
    let info = BurnFuse {
        source_id: b_id,
        object_id: a_id,
        fuse_id: create_id(b"b_battleship_body_fuse"),
        sub_obj_id: SUBOBJ_ID_ROOT,
        at_t: 0.25,
        lifetime: 3.0,
        lit: 1,
    };
    replication::fuse_burnt(&mut cx, he01(), &info);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::BurnFuse(x) => Some(x.clone()),
        _ => None,
    });
    assert_eq!(got, [(1, info.clone()), (3, info)]);

    // A cruise disruptor's list ends an NPC's cruise (`slot_50(false,
    // true, 0)`): the creators hear of it; a ship not cruising, nothing.
    cx.world.command(
        a,
        game_sim::object::Command::EngageCruise {
            engage: true,
            instant: true,
        },
    );
    assert!(replication::cruise_active(cx.world, a));
    let disrupt = |cx: &mut Cx<'_>| {
        let list = DamageList::new(6, b_id, 0);
        let mut hooks = Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.apply_damage_list_hooked(a, &list, &mut hooks);
    };
    disrupt(&mut cx);
    assert!(!replication::cruise_active(cx.world, a));
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateCruise(x) => Some(x.clone()),
        _ => None,
    });
    let ended = ActivateCruise {
        object_id: a_id,
        active: 0,
        field_05: 1,
    };
    assert_eq!(got, [(1, ended.clone()), (3, ended)]);
    disrupt(&mut cx);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateCruise(x) => Some(x.object_id),
        _ => None,
    });
    assert!(got.is_empty());
}

#[test]
fn the_trade_lane_group_and_cargo_notices() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("trade lanes, groups, cargo", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, .. } = scene(&mut cx);
    let a_id = id(&cx, a);
    let _ = sink.take();
    let (ring1, ring2) = (0x1234_5678, 0x0abc_def0);

    // `notify_go_tradelane`: the ship uses the lane; an NPC's skips nobody.
    replication::go_tradelane(&mut cx, he01(), a_id, ring1, ring2, true);
    assert!(cx.world.get(a).unwrap().as_ship().unwrap().using_tradelane);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::GoTradeLane(x) => Some(x.clone()),
        _ => None,
    });
    let go = GoTradeLane {
        object_id: a_id,
        tradelane_ring1: ring1,
        tradelane_ring2: ring2,
        field_0c: 1,
    };
    assert_eq!(got, [(1, go.clone()), (3, go)]);
    replication::stop_tradelane(&mut cx, he01(), a_id, ring1, ring2);
    assert!(!cx.world.get(a).unwrap().as_ship().unwrap().using_tradelane);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::StopTradeLane(x) => Some(x.clone()),
        _ => None,
    });
    let stop = StopTradeLane {
        ship_id: a_id,
        tradelane_ring1: ring1,
        tradelane_ring2: ring2,
    };
    assert_eq!(got, [(1, stop.clone()), (3, stop)]);

    // A player's ship: its owner is not told, though it sees its own ship.
    let own3 = fighter(&mut cx, he01(), 3, Vector::new(15200.0, 3000.0, -25000.0));
    set_ship(&mut cx, 3, own3);
    let own3_id = id(&cx, own3);
    replication::go_tradelane(&mut cx, he01(), own3_id, ring1, ring2, false);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::GoTradeLane(x) => Some(x.object_id),
        _ => None,
    });
    assert!(got.is_empty(), "nobody else has it: {got:?}");

    // `StarSystem_SendGroupChange` through `CPlayerGroup::AddMember`: the
    // owner sees its own ship; player 1's client does not have it.
    let group = cx.logic.groups.create();
    assert!(group::add_member(&mut cx, group, 3));
    let got = server(&sink.take(), |m| match m {
        ServerMessage::MiscObjUpdate(x) if x.flags == 0x09 => Some((x.field_28, x.field_34)),
        _ => None,
    });
    assert_eq!(got, [(3, (own3_id, group))]);
    assert_eq!(
        cx.world.get(own3).unwrap().as_ship().unwrap().group_id,
        group
    );
    assert!(group::del_member(&mut cx, group, 3));
    let got = server(&sink.take(), |m| match m {
        ServerMessage::MiscObjUpdate(x) if x.flags == 0x09 => Some((x.field_28, x.field_34)),
        _ => None,
    });
    assert_eq!(got, [(3, (own3_id, 0))]);
    // An NPC ship's group, to the clients that have it.
    replication::group_changed(&mut cx, he01(), a_id);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::MiscObjUpdate(x) if x.flags == 0x09 => Some(x.field_28),
        _ => None,
    });
    assert_eq!(got, [(1, a_id), (3, a_id)]);

    // `StarSystem_SendRemoteCargoUpdate`: the tracked and created records'
    // clients but the owner's, the items not equipped, flag 1.
    replication::remote_cargo_update(&mut cx, he01(), a);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::RemoteObjectCargoUpdate(x) => Some(x.clone()),
        _ => None,
    });
    assert_eq!(recipients(&got), [1, 3]);
    for (_, update) in &got {
        assert_eq!((update.object_id, update.flag), (a_id, 1));
        assert!(update.records.iter().all(|r| r.equipped == 0));
    }
    // Player 1's client has player 3's ship; the owner is skipped.
    track(&mut cx, he01(), 1, own3);
    replication::remote_cargo_update(&mut cx, he01(), own3);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::RemoteObjectCargoUpdate(x) => Some(x.object_id),
        _ => None,
    });
    assert_eq!(got, [(1, own3_id)]);
}

#[test]
fn a_retargeted_missile_a_lit_fuse_and_a_docking_light_tell_the_creators() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("retargets and fuses", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, b } = scene(&mut cx);
    let (a_id, b_id) = (id(&cx, a), id(&cx, b));
    let _ = sink.take();

    // `Guided::set_target`: the root of the missile, the new pair.
    let missile = cx
        .world
        .create_guided(
            he01(),
            &ProjectileSpawn {
                archetype: create_id(b"missile01_mark01_ammo"),
                owner: b_id,
                position: Vector::new(15000.0, 3000.0, -24800.0),
                orientation: Matrix::IDENTITY,
                velocity: Vector::ZERO,
                target: Some((a, SUBOBJ_ID_ROOT)),
                angular_velocity: Vector::ZERO,
            },
        )
        .expect("the missile");
    let missile_id = id(&cx, missile);
    // Only player 2's client has the missile.
    track(&mut cx, he01(), 2, missile);
    assert_eq!(munitions::set_target(&mut cx, missile, Some(b), 5, 0), 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::SetTarget(x) => Some(x.clone()),
        _ => None,
    });
    let retarget = SetTarget {
        object_id: missile_id,
        target_id: b_id,
        sub_obj_id: SUBOBJ_ID_ROOT,
        target_sub_obj_id: 5,
    };
    assert_eq!(got, [(2, retarget)]);
    // Nothing changed from the server: nothing told.
    assert_eq!(munitions::set_target(&mut cx, missile, Some(b), 5, 0), 0);
    assert!(sink.take().is_empty());

    // `EqObj::light_fuse`: a new fuse carries the lifetime asked for (it
    // destroys no root); its `ignite_fuse` lights the tower in a later
    // burn. Unlit, the nested tower's notice goes first.
    let body = create_id(b"b_battleship_body_fuse");
    let tower = create_id(b"b_battleship_cntrl_twr_fuse");
    assert!(!fuses::fuse_db(&mut cx).get(body).unwrap().has_destroy_root);
    assert!(fuses::light_fuse(
        &mut cx,
        a,
        b_id,
        body,
        SUBOBJ_ID_ROOT,
        0.0,
        30.0
    ));
    let got = server(&sink.take(), |m| match m {
        ServerMessage::BurnFuse(x) => Some(x.clone()),
        _ => None,
    });
    let lit = BurnFuse {
        source_id: b_id,
        object_id: a_id,
        fuse_id: body,
        sub_obj_id: SUBOBJ_ID_ROOT,
        at_t: 0.0,
        lifetime: 30.0,
        lit: 1,
    };
    assert_eq!(got, [(1, lit.clone()), (3, lit)]);
    // Lit again at the same time: no new burn, no notice; at another time
    // the copy's notice carries -1.
    assert!(fuses::light_fuse(
        &mut cx,
        a,
        b_id,
        body,
        SUBOBJ_ID_ROOT,
        0.0,
        30.0
    ));
    assert!(sink.take().is_empty());
    assert!(fuses::light_fuse(
        &mut cx,
        a,
        b_id,
        body,
        SUBOBJ_ID_ROOT,
        0.5,
        30.0
    ));
    let got = server(&sink.take(), |m| match m {
        ServerMessage::BurnFuse(x) => Some((x.at_t, x.lifetime, x.lit)),
        _ => None,
    });
    assert_eq!(got, [(1, (0.5, -1.0, 1)), (3, (0.5, -1.0, 1))]);
    assert!(fuses::unlight_fuse(&mut cx, a, body, SUBOBJ_ID_ROOT, 0.5));
    let _ = sink.take();
    let mut frames = 0;
    while !cx
        .logic
        .objects
        .get(a)
        .unwrap()
        .fuses
        .iter()
        .any(|f| f.fuse.nickname == tower)
    {
        fuses::burn_fuses(&mut cx, a, 0.6);
        frames += 1;
        assert!(frames < 10, "the tower never lit");
    }
    let got = server(&sink.take(), |m| match m {
        ServerMessage::BurnFuse(x) => Some((x.source_id, x.fuse_id, x.lit)),
        _ => None,
    });
    // The `ignite_fuse` passes its burn's source on.
    assert_eq!(got, [(1, (b_id, tower, 1)), (3, (b_id, tower, 1))]);
    assert!(fuses::unlight_fuse(&mut cx, a, body, SUBOBJ_ID_ROOT, 0.0));
    let got = server(&sink.take(), |m| match m {
        ServerMessage::BurnFuse(x) => Some((x.source_id, x.fuse_id, x.lifetime, x.lit)),
        _ => None,
    });
    assert_eq!(
        recipients(&got),
        [1, 3, 1, 3],
        "the tower's, then the body's"
    );
    assert_eq!(got[0].1, (0, tower, -1.0, 0));
    assert_eq!(got[2].1, (0, body, -1.0, 0));

    // `pub::SpaceObj::ActivateEquipment` type 3: each docking light
    // switched tells the players who can see the ship (`msn_playerloadout`
    // has two): player 2's client has it; for the others it is untracked
    // and out of range.
    let spawn = ShipSpawn::new(
        create_id(b"ge_fighter"),
        create_id(b"msn_playerloadout"),
        Vector::new(15000.0, 3000.0, -20000.0),
        Matrix::IDENTITY,
    );
    let lit_ship = cx.world.spawn_ship(he01(), &spawn).expect("ge_fighter");
    let lit_id = id(&cx, lit_ship);
    let on = docking_light_switch(&cx, lit_ship).expect("docking lights");
    track(&mut cx, he01(), 2, lit_ship);
    let far = Vector::new(15000.0, 3000.0, 1.0e6);
    view_mut(&mut cx, he01(), 1).position = far;
    view_mut(&mut cx, he01(), 3).position = far;
    dock::activate_docking_lights(&mut cx, lit_ship, on);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateEquip(x) => Some((x.object_id, x.active)),
        _ => None,
    });
    assert_eq!(got.len(), 2, "both docking lights switched");
    assert!(
        got.iter()
            .all(|&(c, v)| c == 2 && v == (lit_id, u8::from(on)))
    );
    // Already switched: nothing switched, nothing told.
    dock::activate_docking_lights(&mut cx, lit_ship, on);
    assert!(sink.take().is_empty());
}

#[test]
fn comm_reaches_its_addressee_and_broadcasts_within_range() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("comm", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, .. } = scene(&mut cx);
    let a_id = id(&cx, a);
    // Ships 9 km (player 1), 9.9 km (player 2) and 10.1 km (player 3) from
    // the speaker, along x so that the squared distances are exact.
    let own1 = fighter(&mut cx, he01(), 1, Vector::new(24000.0, 3000.0, -25000.0));
    let own2 = fighter(&mut cx, he01(), 2, Vector::new(24900.0, 3000.0, -25000.0));
    let own3 = fighter(&mut cx, he01(), 3, Vector::new(25100.0, 3000.0, -25000.0));
    for (session, ship) in [(1, own1), (2, own2), (3, own3)] {
        set_ship(&mut cx, session, ship);
    }
    let own1_id = id(&cx, own1);
    // Every client has the speaker (player 2's now too), and players 2
    // and 3 have player 1's ship.
    created_for(&mut cx, he01(), 2, a);
    track(&mut cx, he01(), 2, own1);
    track(&mut cx, he01(), 3, own1);
    let _ = sink.take();
    let comm = Comm {
        sender: a_id,
        receiver: own1_id,
        voice: create_id(b"pilot_f_leg_f01"),
        costume: Costume {
            part_ids: [11, 12, 13, 14],
            accessory_ids: vec![21, 22],
        },
        comm_type: 4,
        text_ids: vec![0x100, 0x200, 0x300],
        mood: 0x0001_2345,
        duration: 2.0,
    };
    let sends = |sink: &Shared| {
        server(&sink.take(), |m| match m {
            ServerMessage::SendComm(x) => Some(x.clone()),
            _ => None,
        })
    };

    // To the addressee's player only; nobody after it without a broadcast.
    replication::send_comm(&mut cx, he01(), &comm, false);
    let got = sends(&sink);
    assert_eq!(recipients(&got), [1]);
    let packet = &got[0].1;
    assert_eq!(
        (packet.field_20, packet.field_24, packet.field_28),
        (a_id, own1_id, comm.voice)
    );
    assert_eq!(packet.costume_flag, 1, "the addressee is the player's ship");
    assert_eq!(
        packet.costume,
        WireCostume {
            parts: [11, 12, 13, 14],
            accessories: vec![21, 22],
        }
    );
    assert_eq!(packet.field_60, 4);
    assert_eq!(packet.text_ids, [0x100, 0x200, 0x300]);
    assert_eq!(packet.mood, 0x2345, "the low word");
    // 2.0 × 0.1f × 255 = 51.0000008 in double, truncated.
    assert_eq!(packet.duration, 51);

    // A broadcast: the addressee's player, then the others within 10 km.
    replication::send_comm(&mut cx, he01(), &comm, true);
    let got = sends(&sink);
    assert_eq!(recipients(&got), [1, 2]);
    assert_eq!(got[1].1.costume_flag, 0);

    // No speaker on a multiplayer server: the addressee's player only,
    // unless broadcast.
    let silent = Comm {
        sender: 0,
        ..comm.clone()
    };
    replication::send_comm(&mut cx, he01(), &silent, false);
    assert_eq!(recipients(&sends(&sink)), [1]);
    replication::send_comm(&mut cx, he01(), &silent, true);
    assert_eq!(recipients(&sends(&sink)), [1, 2, 3]);
    // A speaker not in the system: nothing.
    let lost = Comm {
        sender: 0x7fff_fff0,
        ..comm
    };
    replication::send_comm(&mut cx, he01(), &lost, true);
    assert!(sends(&sink).is_empty());
}

#[test]
fn leaving_for_a_dock_latches_the_ship_gone_and_switches_out() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("leaving the system", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, .. } = scene(&mut cx);
    let a_id = id(&cx, a);
    let gate = solar_with(&cx, he01(), 0x40 | 0x800);
    let gate_id = id(&cx, gate);
    let own1 = fighter(&mut cx, he01(), 1, Vector::new(15300.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own1);
    let own1_id = id(&cx, own1);
    track(&mut cx, he01(), 2, own1);
    for session in 1..=3 {
        view_mut(&mut cx, he01(), session)
            .pending_damage
            .insert(gate_id);
    }
    let _ = sink.take();
    let switch_outs = |sink: &Shared| {
        server(&sink.take(), |m| match m {
            ServerMessage::SystemSwitchOut(x) => Some((x.field_00, x.field_04)),
            _ => None,
        })
    };

    // An NPC docks: the clients that had it, with the dock; every record
    // of it goes and every observer forgets its waiting damage.
    dock::complete_dock(&mut cx, he01(), a, gate);
    assert_eq!(
        switch_outs(&sink),
        [(1, (a_id, gate_id)), (3, (a_id, gate_id))]
    );
    for session in 1..=3 {
        let v = view(&cx, he01(), session);
        assert!(v.find(a).is_none());
        assert!(v.pending_damage.is_empty());
        assert!(!v.ship_gone);
    }

    // Player 1's ship docks: its own client (the latch) and player 2's
    // (which had it).
    dock::complete_dock(&mut cx, he01(), own1, gate);
    assert_eq!(
        switch_outs(&sink),
        [(1, (own1_id, gate_id)), (2, (own1_id, gate_id))]
    );
    assert!(view(&cx, he01(), 1).ship_gone);
    assert!(!view(&cx, he01(), 2).ship_gone);

    // Latched, player 1 hears of every object that leaves, and is told of
    // no new one.
    replication::object_left_system(&mut cx, he01(), 0, 0x7fff_fff0, 0);
    let got = switch_outs(&sink);
    assert_eq!(got, [(1, (0x7fff_fff0, 0))]);
    let new = fighter(&mut cx, he01(), 0, Vector::new(15000.0, 3100.0, -25000.0));
    replication::ship_arrived(&mut cx, he01(), new);
    assert!(view(&cx, he01(), 1).find(new).is_none());
    assert!(view(&cx, he01(), 2).find(new).is_some());
}

#[test]
fn a_landing_ship_lands_on_the_clients_that_had_it_and_on_its_owner() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("landing", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Scene { a, b } = scene(&mut cx);
    let (a_id, b_id) = (id(&cx, a), id(&cx, b));
    // Players 1 and 3 have their ships far out, so that the frame creates
    // nothing new.
    let own1 = fighter(&mut cx, he01(), 1, Vector::new(15000.0, 3000.0, 60000.0));
    set_ship(&mut cx, 1, own1);
    let own1_id = id(&cx, own1);
    let base = create_id(b"he01_01_base");
    world::queue_dock(&mut cx, he01(), Some(a), None, base, true);
    world::queue_dock(&mut cx, he01(), Some(b), None, base, false);
    world::queue_dock(&mut cx, he01(), Some(own1), None, base, true);
    let _ = sink.take();
    world::step_system(&mut cx, he01(), 0.02);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::Land(x) => Some((x.field_00, x.field_04, x.field_08)),
        _ => None,
    });
    // The NPC to the clients that had it; the docking without landing to
    // nobody; player 1's ship to its owner, whose client never had it.
    assert_eq!(
        got,
        [
            (1, (a_id, 0, base)),
            (3, (a_id, 0, base)),
            (1, (own1_id, 0, base))
        ]
    );
    assert!(view(&cx, he01(), 2).find(a).is_none(), "every record goes");
    assert!(view(&cx, he01(), 2).find(b).is_some());
    assert!(cx.world.find(b_id).is_some());
    assert!(
        !cx.logic.server_world.system(he01()).unwrap().has_player(1),
        "the landed player left the system"
    );
}

#[test]
fn a_client_object_update_feeds_the_remote_correction_and_the_other_clients() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("object updates", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for session in 1..=3 {
        join(&mut cx, session, he01());
    }
    cx.world.load_system(he01());
    let own1 = fighter(&mut cx, he01(), 1, Vector::new(15000.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own1);
    let own1_id = id(&cx, own1);
    for session in 1..=3 {
        world::add_player(&mut cx, he01(), session);
    }
    created_for(&mut cx, he01(), 3, own1);

    // `RCEnd` in `remove_ship`, `RCBegin` in `add_ship`: the player's ship
    // follows its client while in a system.
    replication::begin_remote_control(&mut cx, own1);
    assert!(cx.world.rc_active(own1));
    let parms = world::remove_ship(&mut cx, own1);
    assert!(parms.is_some() && !cx.world.rc_active(own1));
    let at = Vector::new(15000.0, 3000.0, -24000.0);
    assert!(world::add_ship(&mut cx, he01(), own1, at, Matrix::IDENTITY));
    assert!(cx.world.rc_active(own1));
    created_for(&mut cx, he01(), 3, own1);
    let _ = sink.take();

    // The update: player 3's client has the ship; player 2's record takes
    // the sample all the same; the sender hears nothing.
    let orientation = Quaternion {
        w: 0.6,
        x: 0.8,
        y: 0.0,
        z: 0.0,
    };
    let throttle = 0.5f32;
    let update = ObjectUpdate {
        object_id: own1_id,
        orientation,
        position: Vector::new(15000.0, 3000.0, -23990.0),
        timestamp: 12.25,
        field_28: throttle.to_bits(),
        field_2c: 3,
    };
    let samples = |cx: &Cx<'_>, session: u32| {
        view(cx, he01(), session)
            .find(own1)
            .unwrap()
            .simulation
            .sample_count()
    };
    let before = samples(&cx, 2);
    replication::client_object_update(&mut cx, &update, 1);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::UpdateObject(x) => Some(x.clone()),
        _ => None,
    });
    assert_eq!(recipients(&got), [3]);
    let packet = &got[0].1;
    assert_eq!(packet.object_id, own1_id);
    assert_eq!(packet.flags, 3);
    assert_eq!(packet.position, [15000.0, 3000.0, -23990.0]);
    assert_eq!(
        packet.orientation,
        quantise_orientation([0.6, 0.8, 0.0, 0.0])
    );
    assert_eq!(packet.extra, 63, "0.5 × 127, truncated");
    assert_eq!(packet.update_time, 12.25);
    assert_eq!(samples(&cx, 2), before + 1);
    assert_eq!(
        view(&cx, he01(), 2)
            .find(own1)
            .unwrap()
            .simulation
            .latest_update(),
        update.position
    );

    // Past 750 km from the origin the ship is destroyed and nothing goes.
    let wild = ObjectUpdate {
        position: Vector::new(800_000.0, 0.0, 0.0),
        ..update
    };
    replication::client_object_update(&mut cx, &wild, 1);
    let sent = sink.take();
    assert!(
        common(&sent, |m| match m {
            CommonMessage::UpdateObject(x) => Some(x.object_id),
            _ => None,
        })
        .is_empty()
    );
    assert!(cx.logic.objects.get(own1).is_none_or(|o| o.destroyed));
}

#[test]
fn the_changed_affiliations_go_out_after_the_frame() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("changed affiliations", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for session in 1..=3 {
        join(&mut cx, session, he01());
    }
    cx.world.load_system(he01());
    // Within 5000 m of the origin, where player 2 (without a ship) views
    // from: the frame keeps it on player 2's client.
    let own1 = fighter(&mut cx, he01(), 1, Vector::new(0.0, 3000.0, 0.0));
    set_ship(&mut cx, 1, own1);
    let own1_id = id(&cx, own1);
    for session in 1..=3 {
        world::add_player(&mut cx, he01(), session);
    }
    created_for(&mut cx, he01(), 2, own1);
    let _ = sink.take();
    {
        let (vibes, _) = cx.world.vibes();
        vibes.changed_affiliations.insert(1, 7);
        vibes.changed_affiliations.insert(3, 8);
    }
    let affiliation = |cx: &mut Cx<'_>, session: u32| {
        let vibe = cx.logic.players.get(session).unwrap().rep_vibe;
        cx.world.vibes().0.affiliation(vibe, false).1
    };
    let (aff1, aff3) = (affiliation(&mut cx, 1), affiliation(&mut cx, 3));
    world::step_system(&mut cx, he01(), 0.02);
    let got = server(&sink.take(), |m| match m {
        ServerMessage::MiscObjUpdate(x) if x.flags == 0x28 => Some((x.field_28, x.field_54)),
        _ => None,
    });
    // Player 1's ship to the players who can see it: its owner, player 2,
    // and player 3, whose client the frame gave the ship (both view from
    // the origin); player 3 has no ship, so only it hears, of object 0.
    assert!(view(&cx, he01(), 3).find(own1).unwrap().created_on_client);
    assert_eq!(
        got,
        [
            (1, (own1_id, aff1)),
            (2, (own1_id, aff1)),
            (3, (own1_id, aff1)),
            (3, (0, aff3))
        ]
    );
    assert!(cx.world.vibes().0.changed_affiliations.is_empty());
    // The map is the world's: the next frame sends nothing.
    world::step_system(&mut cx, he01(), 0.02);
    assert!(
        server(&sink.take(), |m| match m {
            ServerMessage::MiscObjUpdate(x) if x.flags == 0x28 => Some(()),
            _ => None,
        })
        .is_empty()
    );
}

#[test]
fn single_player_tells_every_observer_and_runs_no_remote_correction() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("single player's notices", true, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    join(&mut cx, 1, he02());
    cx.world.load_system(he02());
    cx.world.load_system_solars(he02());
    let npc = fighter(&mut cx, he02(), 0, Vector::new(0.0, 3000.0, 90000.0));
    let own = fighter(&mut cx, he02(), 1, Vector::new(0.0, 3000.0, -25000.0));
    set_ship(&mut cx, 1, own);
    world::add_player(&mut cx, he02(), 1);
    let _ = sink.take();
    let npc_id = id(&cx, npc);
    replication::cruise_changed(&mut cx, he02(), npc_id, false, true, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::ActivateCruise(x) => Some(x.clone()),
        _ => None,
    });
    assert_eq!(
        got,
        [(
            1,
            ActivateCruise {
                object_id: npc_id,
                active: 0,
                field_05: 1,
            }
        )]
    );
    // The aim travels as floats in single player.
    let aim = Vector::new(1.5, 2.5, 3.5);
    replication::weapon_fired(&mut cx, he02(), npc_id, &[0x22], aim, 0);
    let got = common(&sink.take(), |m| match m {
        CommonMessage::FireWeapon(x) => Some(x.target),
        _ => None,
    });
    assert_eq!(got, [(1, [1.5, 2.5, 3.5].map(FireComponent::Full))]);
    replication::begin_remote_control(&mut cx, own);
    assert!(!cx.world.rc_active(own));
    // No update fans out in single player.
    let update = ObjectUpdate {
        object_id: id(&cx, own),
        orientation: Quaternion {
            w: 1.0,
            x: 0.0,
            y: 0.0,
            z: 0.0,
        },
        position: Vector::ZERO,
        timestamp: 1.0,
        field_28: 0,
        field_2c: 0,
    };
    replication::client_object_update(&mut cx, &update, 1);
    assert!(sink.take().is_empty());
}
