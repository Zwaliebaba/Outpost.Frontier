//! The server's links that P3.11d ports, over the original data
//! (`outdir/`, he01; each test returns early with a notice when it is
//! missing):
//!
//! - a tractor's consumed arm (`SrvTractor::notify` event 0x18,
//!   `StarSystem::tractor_loot`): the loot into the hold, `LootAcquired`,
//!   `ITEMTRACTORED`, the arm let go and the loot destroyed, or the rest
//!   of a stack left in space;
//! - a field mine's blast (`MedMine::update`, `StarSystem::
//!   send_mine_explosion`): `EXPLODEASTEROIDMINE` to the players that see
//!   the mine;
//! - the behaviour manager's events 3 (`GameObject::on_object_left_system`),
//!   5 (`pub::SpaceObj::Dock`) and 7 (`Ship::light_fuse`, `EqObj::destroy`);
//! - a blast's damage to objects without equipment
//!   (`GameObject::explosion_hit`): loot, mines, counter-measures and
//!   guided munitions;
//! - the server's frame keeps neither the world's event log nor the beam
//!   diagnostics.
//!
//! Expected values follow the C++ (`Server/server_classes.cpp`,
//! `server_gameobject.cpp`, `server_pub.cpp`,
//! `Common/common_behaviormanager.cpp`).

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::ai::{self, BaseOp, CallbackArg, Op, OpRType, SetPersonalityParams};
use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::events::ControllerMessage;
use game_logic::loot::LootState;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world::{self, Hooks};
use game_logic::{equip_fire, fuses, munitions};
use game_sim::ai::behavior::{DirectiveOp, FollowOp, ids};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{EquipEvent, EquipKind, SUBOBJ_ID_NONE, SubObjId, types};
use game_sim::object::damage::{self, DamageList};
use game_sim::object::world::{Assets, LootSpawn, ProjectileSpawn, ShipSpawn, WorldConfig};
use game_sim::object::{KindData, ObjHandle, World};
use game_sim::protocol::ToClient;
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

fn setup(what: &str, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let logic = Logic::new(Box::new(sink.clone()), 2, true);
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// he01 loaded by the server (its solars and fields) with Content's
/// GameSupport, which owns the ships, and player 2 in the system.
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

/// Player 2's ship at `at` (`ship_object_id` set), so that its view sees
/// what is near it.
fn player_ship(cx: &mut Cx<'_>, at: Vector) -> ObjHandle {
    let mut spawn = ShipSpawn::new(create_id(b"ge_fighter"), 0, at, Matrix::IDENTITY);
    spawn.owner_player = 2;
    let h = cx
        .world
        .spawn_ship(he01(), &spawn)
        .expect("ge_fighter spawns");
    if let Some(s) = cx.world.get_mut(h).and_then(|o| o.as_ship_mut()) {
        s.is_player = true;
    }
    let id = cx.world.get(h).unwrap().simple.id;
    cx.logic.players.get_mut(2).unwrap().ship_object_id = id;
    h
}

/// An NPC fighter made through `pub::SpaceObj::Create` at `at` (its
/// loadout has a tractor).
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

/// The fighter with the `FIGHTER` state graph and a current behaviour.
fn pilot(cx: &mut Cx<'_>, controller: u32, at: Vector) -> (ObjHandle, u32) {
    let (h, id) = fighter(cx, controller, at);
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
    assert_eq!(ai::submit_state(cx, id, &op), OpRType::SUCCESS);
    (h, id)
}

fn frames(cx: &mut Cx<'_>, n: usize) {
    for _ in 0..n {
        world::step_system(cx, he01(), 0.05);
        cx.world.update_global_time(0.05);
    }
}

fn id_of(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).map_or(0, |o| o.simple.id)
}

fn behavior(cx: &Cx<'_>, h: ObjHandle) -> i32 {
    cx.world
        .behavior_manager(h)
        .map_or(-1, |m| m.state.behavior)
}

fn first_item(cx: &Cx<'_>, h: ObjHandle, mask: u32) -> SubObjId {
    cx.world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.first(mask))
        .map(|e| e.id)
        .expect("the item")
}

/// The ship's stack of `arch`: its id, count and hit points.
fn stack(cx: &Cx<'_>, h: ObjHandle, arch: u32) -> Option<(SubObjId, u32, f32)> {
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    eq.equipment
        .traverse(types::CARGO, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot))
        .find(|e| e.arch_id == arch)
        .and_then(|e| match &e.kind {
            EquipKind::Cargo(c) => Some((e.id, c.count, c.hit_pts)),
            _ => None,
        })
}

/// A `LootAcquired`: the loot, the ship, the stack, the good, the units.
type Acquired = (u32, u32, u16, u32, i32);

thread_local! {
    static ACQUIRED: RefCell<Vec<Acquired>> = const { RefCell::new(Vec::new()) };
}

/// A delivery hook that notes each `LootAcquired` (the bus delivers the
/// broadcast once per controller).
fn acquired_hook(
    _: &mut Cx<'_>,
    _: neuron_server::controller::ControllerId,
    _: game_logic::server::ControllerKind,
    message: &mut ControllerMessage<'_>,
) {
    if let ControllerMessage::LootAcquired {
        loot,
        ship,
        sub_object,
        good,
        count,
    } = *message
    {
        ACQUIRED.with(|a| a.borrow_mut().push((loot, ship, sub_object, good, count)));
    }
}

const LUXURY_FOOD: &[u8] = b"commodity_luxury_food";

/// A container of `units` of luxury food (100 hit points each, decaying
/// 1 per second) at `at`, with its server entry.
fn food(cx: &mut Cx<'_>, at: Vector, units: u32) -> ObjHandle {
    let h = cx
        .world
        .spawn_loot(
            he01(),
            &LootSpawn {
                archetype: create_id(b"lootcrate_red"),
                contents: create_id(LUXURY_FOOD),
                units,
                contents_hit_pts: 100.0,
                position: at,
                hit_pts: -1.0,
                orientation: Matrix::IDENTITY,
                can_ai_tractor: true,
                ..LootSpawn::default()
            },
        )
        .expect("the loot spawns");
    let _ = cx.logic.objects.entry(cx.world, h);
    h
}

#[test]
fn a_consumed_arm_takes_the_loot_into_the_hold() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("tractored loot", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let at = Vector::new(15000.0, 3000.0, -25000.0);
    let (ship, ship_id) = fighter(&mut cx, controller, at);
    let _ = player_ship(&mut cx, Vector::new(15050.0, 3000.0, -25000.0));
    let tractor = first_item(&cx, ship, types::TRACTOR);
    let arch = create_id(LUXURY_FOOD);
    frames(&mut cx, 1);
    let space = cx.world.space_for_cargo_type(ship, arch).unwrap();
    assert!(space > 3, "the hold has room: {space}");

    // The whole stack: 5 units whose decay took 30 of the contents' 100
    // hit points carry 0.7 into the hold.
    let loot = food(&mut cx, Vector::new(15000.0, 3000.0, -24980.0), 5);
    let loot_id = id_of(&cx, loot);
    cx.logic.objects.entry(cx.world, loot).unwrap().loot = Some(LootState {
        check_timer: 0.0,
        unseen_life: 100.0,
        decay_accum: 30.0,
    });
    assert_eq!(
        cx.world.tractor_add_target(ship, tractor, loot),
        game_sim::equip::misc::TractorFailure::Ok
    );
    sink.take();
    cx.logic.deliver_hook = Some(acquired_hook);
    ACQUIRED.with(|a| a.borrow_mut().clear());
    equip_fire::equip_event(
        &mut cx,
        ship,
        tractor,
        EquipEvent::TractorConsume(Some(loot)),
    );
    let (stack_id, count, hit_pts) = stack(&cx, ship, arch).expect("the food is in the hold");
    assert_eq!(count, 5);
    assert!((hit_pts - 70.0).abs() < 1e-3, "{hit_pts}");
    let good = cx.world.data().goods.arch_to_good(arch);
    let acquired = ACQUIRED.with(|a| std::mem::take(&mut *a.borrow_mut()));
    assert!(!acquired.is_empty(), "LootAcquired reaches the controllers");
    assert!(
        acquired
            .iter()
            .all(|&a| a == (loot_id, ship_id, stack_id, good, 5)),
        "{acquired:?}"
    );
    let tractored: Vec<(u32, u32, u32, u16, u16)> = sink
        .take()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::ItemTractored(t) => {
                Some((c, t.loot_id, t.ship_id, t.tractor_id, t.units))
            }
            _ => None,
        })
        .collect();
    assert_eq!(tractored, [(2, loot_id, ship_id, tractor, 5)]);
    assert!(
        cx.logic.objects.get(loot).is_some_and(|o| o.destroyed)
            || cx.world.get(loot).is_none_or(|o| o.dead),
        "the loot is destroyed"
    );
    assert!(match cx.world.get(loot).map(|o| &o.data) {
        Some(KindData::Loot(l)) => l.units == 0,
        _ => true,
    });
    // The arm let go: the tractor has no target left and is off.
    let active = cx
        .world
        .get(ship)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.by_id(tractor))
        .is_some_and(game_sim::equip::Equip::is_active);
    assert!(!active, "the tractor deactivates with no arm left");

    // More units than the room: the room's worth goes, the rest stays in
    // space, and the arm keeps its loot.
    let space = cx.world.space_for_cargo_type(ship, arch).unwrap() as u32;
    let big = food(&mut cx, Vector::new(15000.0, 3000.0, -24980.0), space + 3);
    assert_eq!(
        cx.world.tractor_add_target(ship, tractor, big),
        game_sim::equip::misc::TractorFailure::Ok
    );
    equip_fire::equip_event(
        &mut cx,
        ship,
        tractor,
        EquipEvent::TractorConsume(Some(big)),
    );
    assert_eq!(stack(&cx, ship, arch).map(|s| s.1), Some(5 + space));
    assert!(matches!(
        cx.world.get(big).map(|o| &o.data),
        Some(KindData::Loot(l)) if l.units == 3
    ));
    assert!(cx.logic.objects.get(big).is_none_or(|o| !o.destroyed));
    // A full hold takes nothing.
    assert_eq!(cx.world.space_for_cargo_type(ship, arch), Some(0));
    sink.take();
    equip_fire::equip_event(
        &mut cx,
        ship,
        tractor,
        EquipEvent::TractorConsume(Some(big)),
    );
    assert!(sink.take().is_empty());
}

#[test]
fn a_tractor_pulls_its_loot_in_over_the_frames() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a tractor's frames", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (ship, ship_id) = fighter(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let _ = player_ship(&mut cx, Vector::new(15050.0, 3000.0, -25000.0));
    let tractor = first_item(&cx, ship, types::TRACTOR);
    let loot = food(&mut cx, Vector::new(15000.0, 3000.0, -24940.0), 4);
    let loot_id = id_of(&cx, loot);
    assert_eq!(
        cx.world.tractor_add_target(ship, tractor, loot),
        game_sim::equip::misc::TractorFailure::Ok
    );
    cx.logic.deliver_hook = Some(acquired_hook);
    ACQUIRED.with(|a| a.borrow_mut().clear());
    let mut frame = 0;
    while cx.world.get(loot).is_some_and(|o| !o.dead) && frame < 200 {
        frames(&mut cx, 1);
        frame += 1;
    }
    println!("the loot was taken after {frame} frames");
    assert!(frame < 200, "the arm consumed the loot");
    assert_eq!(
        stack(&cx, ship, create_id(LUXURY_FOOD)).map(|s| s.1),
        Some(4)
    );
    let acquired = ACQUIRED.with(|a| std::mem::take(&mut *a.borrow_mut()));
    assert!(
        !acquired.is_empty()
            && acquired
                .iter()
                .all(|a| (a.0, a.1, a.4) == (loot_id, ship_id, 4)),
        "{acquired:?}"
    );
    assert!(sink.take().iter().any(|(c, m)| *c == 2
        && matches!(m, ServerMessage::ItemTractored(t) if t.loot_id == loot_id && t.units == 4)));
}

#[test]
fn a_field_mine_tells_the_players_that_see_it() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("field mines", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    // he01's Zone_he01_zone21 is filled with mine_spike1 mines; 8 km south
    // of its centre the cubes fill (game-sim's `world_data`).
    let at = Vector::new(81640.0, 0.0, -5950.0);
    let _ = fighter(&mut cx, controller, at);
    let _ = player_ship(&mut cx, Vector::new(81660.0, 0.0, -5950.0));
    sink.take();
    let mut blown = Vec::new();
    for _ in 0..150 {
        frames(&mut cx, 1);
        blown.extend(sink.take().into_iter().filter_map(|(c, m)| match m {
            ServerMessage::ExplodeAsteroidMine(x) => Some((c, x.field_00, x.position)),
            _ => None,
        }));
        if !blown.is_empty() {
            break;
        }
    }
    println!("mine explosions: {blown:?}");
    let &(client, mine, offset) = blown.first().expect("a field mine blew");
    assert_eq!(client, 2);
    let h = cx.world.find(mine).expect("the mine");
    let Some(KindData::Asteroid(a)) = cx.world.get(h).map(|o| &o.data) else {
        panic!("not an asteroid");
    };
    let arm = a.armed.as_ref().expect("an armed field mine");
    let length = (f64::from(offset[0]).powi(2)
        + f64::from(offset[1]).powi(2)
        + f64::from(offset[2]).powi(2))
    .sqrt();
    assert!(
        (length - f64::from(arm.explosion_offset.abs())).abs() < 1e-3,
        "the offset is the explosion offset along the intruder's direction: {length}"
    );
}

#[test]
fn a_blast_damages_loot_and_munitions() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("blasts", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    he01_with_a_player(&mut cx);
    let center = Vector::new(15000.0, 3000.0, 0.0);
    let near = |dx: f32| Vector::new(center.x + dx, center.y, center.z);
    let spawn = |arch: &[u8], at: Vector| ProjectileSpawn {
        archetype: create_id(arch),
        owner: 0,
        position: at,
        orientation: Matrix::IDENTITY,
        velocity: Vector::ZERO,
        target: None,
        angular_velocity: Vector::ZERO,
    };
    let mine = munitions::create_mine(&mut cx, he01(), &spawn(b"mine01_mark01_ammo", near(20.0)))
        .expect("the mine");
    let cm =
        munitions::create_counter_measure(&mut cx, he01(), &spawn(b"ge_s_cm_01_ammo", near(-20.0)))
            .expect("the counter-measure");
    let missile = equip_fire::create_guided(
        &mut cx,
        he01(),
        &equip_fire::GuidedLaunch {
            spawn: spawn(b"missile01_mark01_ammo", near(40.0)),
            equip: SUBOBJ_ID_NONE,
            target_id: 0,
            target_sub: 0,
        },
    )
    .expect("the missile");
    let loot = food(&mut cx, near(-40.0), 2);
    let before = cx.world.get(loot).unwrap().simple.hit_pts;
    let blast = damage::Blast {
        position: center,
        radius: 300.0,
        hull_damage: 150.0,
        energy_damage: 0.0,
    };
    let inflictor = 0x1234;
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world
        .explode_caused_hooked(he01(), &blast, 5, inflictor, None, 0.0, &mut hooks);
    // `GameObject::explosion_hit`: the loot's physical sphere is in the
    // inner band, so it takes `((3 * 1/3) * 150) * resistance`.
    let (radius, c) = cx.world.physical_radius_r(loot);
    let d = [
        f64::from(center.x) - f64::from(c.x),
        f64::from(center.y) - f64::from(c.y),
        f64::from(center.z) - f64::from(c.z),
    ];
    let dist = ((d[2] * d[2] + d[0] * d[0]) + d[1] * d[1]) - f64::from(radius) * f64::from(radius);
    let inner = (f64::from(300.0f32 * 300.0) * f64::from(1.0f32 / 9.0)) as f32;
    assert!(dist < f64::from(inner), "the loot is in the inner band");
    let resistance = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&cx.world.get(loot).unwrap().archetype)
        .unwrap()
        .root
        .explosion_resistance;
    let amount = ((3.0 * f64::from(1.0f32 / 3.0)) * 150.0 * f64::from(resistance)) as f32;
    let want = (f64::from(before) - f64::from(amount)) as f32;
    let after = cx.world.get(loot).map(|o| o.simple.hit_pts);
    println!("loot {before} -> {after:?} (resistance {resistance}, amount {amount})");
    if want > 0.0 {
        assert_eq!(after, Some(want));
    } else {
        assert!(cx.world.get(loot).is_none_or(|o| o.dead));
    }
    // The mine, the counter-measure and the missile have fewer hit points
    // than the blast takes: each is destroyed by the inflictor.
    for (what, h) in [
        ("mine", mine),
        ("counter-measure", cm),
        ("missile", missile),
    ] {
        let destroyed = cx.logic.objects.get(h).is_some_and(|o| o.destroyed)
            || cx.world.get(h).is_none_or(|o| o.dead);
        assert!(destroyed, "the {what} is destroyed");
    }
    let ids = [id_of(&cx, mine), id_of(&cx, cm), id_of(&cx, missile)];
    let killed: Vec<(u32, u32)> = cx
        .world
        .take_events()
        .into_iter()
        .filter_map(|e| match e {
            game_sim::object::world::WorldEvent::Destroyed { id, killer } if ids.contains(&id) => {
                Some((id, killer))
            }
            _ => None,
        })
        .collect();
    println!("destroyed (id, killer): {killed:?}; mine, counter-measure, missile: {ids:?}");
    // In the system's order the missile goes first, by the inflictor; its
    // own blast (`Guided::destroy`, from its owner, none) destroys the
    // mine inside the pass, as the reference's nested `explode` does; the
    // counter-measure is the blast's.
    assert_eq!(
        killed,
        [(ids[2], inflictor), (ids[0], 0), (ids[1], inflictor)]
    );
}

#[test]
fn a_ship_leaving_its_system_is_let_go() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("event 3", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (leader, leader_id) = pilot(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let (wing, wing_id) = pilot(&mut cx, controller, Vector::new(14955.0, 3000.0, -24986.0));
    frames(&mut cx, 2);
    assert_ne!(behavior(&cx, leader), -1);
    assert_eq!(
        ai::update_formation_state(&mut cx, leader_id, leader_id, Vector::ZERO),
        Ok(())
    );
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, leader_id, Vector::new(-45.0, 0.0, 14.0)),
        Ok(())
    );
    let followers = |cx: &Cx<'_>| {
        cx.world
            .get(leader)
            .unwrap()
            .as_ship()
            .unwrap()
            .formation_followers
            .clone()
    };
    assert_eq!(followers(&cx), [Some(wing)]);
    // `StarSystem::remove_ship`: the leader's manager hears the event 3 and
    // its ship lets go of the follower (`CShip::leaving_system_update`).
    let _ = world::remove_ship(&mut cx, wing);
    assert_eq!(followers(&cx), [None]);
}

#[test]
fn a_dock_answer_reaches_the_follow_followers() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("event 5", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (ship, ship_id) = pilot(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let (follower, follower_id) =
        pilot(&mut cx, controller, Vector::new(15000.0, 3000.0, -24500.0));
    let follow = BaseOp::directive(DirectiveOp::Follow(FollowOp {
        target: ship_id,
        ..FollowOp::default()
    }));
    assert_eq!(
        ai::submit_directive(&mut cx, follower_id, &follow),
        OpRType::ACCEPTED
    );
    assert_eq!(ai::behavior_id(&mut cx, follower_id), ids::FOLLOW);
    frames(&mut cx, 2);
    let follows = cx
        .world
        .get(ship)
        .unwrap()
        .as_ship()
        .unwrap()
        .follow_followers
        .contains(&Some(follower));
    assert!(follows, "the leader lists its follow follower");
    let station = create_id(b"he01_11");
    let station_type = cx
        .world
        .find(station)
        .and_then(|h| cx.world.get(h))
        .map_or(0, |o| o.simple.type_bits);
    assert_ne!(station_type & 0x120, 0, "a station: {station_type:#x}");
    let parked = |cx: &Cx<'_>| {
        cx.world
            .behavior_manager(follower)
            .unwrap()
            .state
            .sys
            .formation
            .value_b4
    };
    assert_eq!(parked(&cx), (0, false));
    // Content's answer with a port: the follower's formation block parks
    // the station and the port (`SetValueB4`).
    let _ = space_obj::dock(
        &mut cx,
        ship_id,
        station,
        0,
        game_logic::dock::RESPONSE_PROCEED,
    );
    assert_eq!(parked(&cx), (station, true));
    // A waiting answer stops before the followers; an answer without a
    // port parks the station without one.
    let _ = space_obj::dock(
        &mut cx,
        ship_id,
        station,
        -1,
        game_logic::dock::RESPONSE_WAIT,
    );
    assert_eq!(parked(&cx), (station, true));
    let _ = space_obj::dock(
        &mut cx,
        ship_id,
        station,
        -1,
        game_logic::dock::RESPONSE_PROCEED,
    );
    assert_eq!(parked(&cx), (station, false));
}

#[test]
fn a_lit_death_fuse_reaches_the_followers_and_the_ship() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("event 7", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (leader, leader_id) = pilot(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let (wing, wing_id) = pilot(&mut cx, controller, Vector::new(14955.0, 3000.0, -24986.0));
    frames(&mut cx, 2);
    assert_eq!(
        ai::update_formation_state(&mut cx, leader_id, leader_id, Vector::ZERO),
        Ok(())
    );
    assert_eq!(
        ai::update_formation_state(&mut cx, wing_id, leader_id, Vector::new(-45.0, 0.0, 14.0)),
        Ok(())
    );
    frames(&mut cx, 1);
    let suppressed = |cx: &Cx<'_>, h: ObjHandle| {
        cx.world
            .behavior_manager(h)
            .unwrap()
            .state
            .sys
            .formation
            .leader_suppressed
    };
    assert!(!suppressed(&cx, wing));
    assert_ne!(behavior(&cx, wing), -1);
    // `Ship::light_fuse` of `death_comm`: the follower's manager hides its
    // leaders (`SuppressCurrentLeader`).
    assert!(fuses::light_fuse(
        &mut cx,
        leader,
        0,
        fuses::death_comm_fuse(),
        SUBOBJ_ID_NONE,
        0.0,
        5.0
    ));
    assert!(suppressed(&cx, wing));
    assert!(!suppressed(&cx, leader));
    // `EqObj::destroy`'s own event 7: the ship's manager takes `Field120`
    // and stops its ticks.
    let fallback = cx.world.behavior_manager(leader).unwrap().state.f120;
    cx.world.fuse_event(leader, leader_id);
    let m = cx.world.behavior_manager(leader).unwrap();
    assert!(m.state.suppressed);
    assert_eq!(m.state.behavior, fallback);
}

#[test]
fn the_server_frame_keeps_no_event_log_or_beam_lists() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the frame's logs", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = he01_with_a_player(&mut cx);
    let (shooter, _) = fighter(&mut cx, controller, Vector::new(15000.0, 3000.0, -25000.0));
    let (_, target_id) = fighter(&mut cx, controller, Vector::new(15000.0, 3000.0, -24900.0));
    let guns = cx.world.unguided_guns(shooter);
    for _ in 0..20 {
        cx.world.command(
            shooter,
            game_sim::object::Command::Fire {
                guns: guns.clone(),
                target: Vector::new(15000.0, 3000.0, -24900.0),
            },
        );
        world::step_system(&mut cx, he01(), 0.05);
        cx.world.update_global_time(0.05);
        assert!(cx.world.take_beam_starts_inside(he01()).is_empty());
        assert!(cx.world.take_beam_ov_culled(he01()).is_empty());
    }
    assert!(cx.world.find(target_id).is_some());
    let _ = DamageList::new(0, 0, 0);
    // The world logs its events until the server's frame ends.
    cx.world.command(
        shooter,
        game_sim::object::Command::Fire {
            guns,
            target: Vector::new(15000.0, 3000.0, -24900.0),
        },
    );
    world::physics_update(&mut cx, 0.05);
    assert!(cx.world.take_events().is_empty());
}
