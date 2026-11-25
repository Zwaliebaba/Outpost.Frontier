//! The server's mines, counter-measures and guided munitions over the
//! original data (`outdir/`): `StarSystem::create_mine` and `Mine::arm`,
//! the mine's lifetime, its phantom (`added`, `deleted`), the owner's
//! exemption, seeking and detonation and its explosion; the
//! counter-measure's diversion of missiles and its explosion; the guided
//! munition's `set_target` and the counter-measure's kill without a
//! detonation; and the PvP gate. Expected values follow the C++
//! (`Server/server_mine.cpp`, `server_countermeasure.cpp`,
//! `server_guided.cpp`, `server_classes.cpp`).

use std::path::Path;
use std::sync::Arc;

use game_logic::munitions::{self, MineState};
use game_logic::server::{Cx, Logic, RecordingSink};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{
    Assets, ControllerRef, ProjectileSpawn, ShipSpawn, WorldConfig, WorldEvent,
};
use game_sim::object::{KindData, ObjHandle, World, csys};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::PhantomEvent;
use neuron_core::vfs::Host;

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The data, with `edit` applied before the world shares it.
fn data_with(what: &str, edit: impl FnOnce(&mut GameData)) -> Option<Arc<GameData>> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    edit(&mut data);
    Some(Arc::new(data))
}

fn setup_with(what: &str, edit: impl FnOnce(&mut GameData)) -> Option<(World, Logic)> {
    let data = data_with(what, edit)?;
    let mut world =
        World::new(data, WorldConfig::default()).with_assets(Assets::new(outdir().join("data")));
    world.load_system(he01());
    let logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    Some((world, logic))
}

fn setup(what: &str) -> Option<(World, Logic)> {
    setup_with(what, |_| {})
}

fn he01() -> u32 {
    create_id(b"he01")
}

const BASE: Vector = Vector::new(15000.0, 3000.0, 0.0);

fn at(x: f32, y: f32, z: f32) -> Vector {
    Vector::new(BASE.x + x, BASE.y + y, BASE.z + z)
}

fn fighter(cx: &mut Cx<'_>, position: Vector) -> ObjHandle {
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        position,
        Matrix::IDENTITY,
    );
    cx.world
        .spawn_ship(he01(), &spawn)
        .expect("he_fighter spawns")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).map_or(0, |o| o.simple.id)
}

fn launch(archetype: &[u8], owner: u32, position: Vector, velocity: Vector) -> ProjectileSpawn {
    ProjectileSpawn {
        archetype: create_id(archetype),
        owner,
        position,
        orientation: Matrix::IDENTITY,
        velocity,
        target: None,
        angular_velocity: Vector::ZERO,
    }
}

const MINE: &[u8] = b"mine01_mark01_ammo";
const COUNTER_MEASURE: &[u8] = b"ge_s_cm_01_ammo";
const MISSILE: &[u8] = b"missile01_mark01_ammo";

fn mine_data(cx: &Cx<'_>, h: ObjHandle) -> game_sim::object::projectile::MineData {
    match cx.world.get(h).map(|o| &o.data) {
        Some(KindData::Mine(m)) => m.clone(),
        _ => panic!("not a mine"),
    }
}

fn mine_state(cx: &Cx<'_>, h: ObjHandle) -> MineState {
    cx.logic
        .objects
        .get(h)
        .and_then(|o| o.mine.clone())
        .expect("the mine's server state")
}

/// One server frame of he01 and the global time after it.
fn frame(cx: &mut Cx<'_>, dt: f32) {
    world::step_system(cx, he01(), dt);
    cx.world.update_global_time(dt);
}

fn destroyed(events: &[WorldEvent], who: u32) -> Option<u32> {
    events.iter().find_map(|e| match e {
        WorldEvent::Destroyed { id, killer } if *id == who => Some(*killer),
        _ => None,
    })
}

/// The ship's hull plus its first shield's hit points.
fn health(cx: &Cx<'_>, h: ObjHandle) -> f32 {
    let Some(o) = cx.world.get(h) else {
        return 0.0;
    };
    let shield = o
        .equipped
        .as_deref()
        .and_then(|eq| eq.equipment.first(game_sim::equip::types::SHIELD))
        .and_then(|e| match &e.kind {
            game_sim::equip::EquipKind::Shield(s) => Some(s.hit_pts),
            _ => None,
        })
        .unwrap_or(0.0);
    o.simple.hit_pts + shield
}

#[test]
fn create_mine_draws_its_spin_and_arms_a_seeking_mine() {
    let Some((mut world, mut logic)) = setup("mine creation") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let velocity = Vector::new(3.0, -4.0, 12.0);
    // `random_inside_sphere(|v|)`: three draws of the CRT generator.
    let mut expected = cx.world.rng().clone();
    let _ = csys::random_inside_sphere(13.0, &mut expected);
    let h = munitions::create_mine(
        &mut cx,
        he01(),
        &launch(MINE, 77, at(0.0, 0.0, 0.0), velocity),
    )
    .expect("the mine is made");
    assert_eq!(*cx.world.rng(), expected, "three draws");

    // The archetype seeks (seek_dist 400): its controller and a phantom
    // of the seek distance, collisions off until its first update.
    let m = mine_data(&cx, h);
    assert_eq!((m.seek_dist, m.detonation_dist), (400.0, 4.0));
    assert_eq!(m.projectile.owner_id, 77);
    let phantom = m.phantom.expect("a phantom");
    let o = cx.world.get(h).unwrap();
    assert!(
        o.physical
            .controllers
            .iter()
            .any(|(which, _, _)| *which == ControllerRef::Mine)
    );
    let physics = cx.world.physics(he01()).unwrap();
    assert!(!physics.phantom_collision_state(phantom));
    assert!(!m.seeking);
    assert_eq!(mine_state(&cx, h), MineState::default());
    assert_eq!(MineState::default().seek_timer, f32::MAX);

    // `arm` without a seek distance: a detonation distance takes the seek
    // distance (0: no phantom), a negative one makes none (D630), and
    // neither makes nothing; none registers the controller.
    for (seek, detonation) in [(0.0, 4.0), (-5.0, 4.0), (0.0, 0.0)] {
        let plain = cx
            .world
            .create_mine(he01(), &launch(MINE, 77, at(0.0, 500.0, 0.0), Vector::ZERO))
            .unwrap();
        if let Some(KindData::Mine(m)) = cx.world.get_mut(plain).map(|o| &mut o.data) {
            m.seek_dist = seek;
            m.detonation_dist = detonation;
        }
        munitions::arm(&mut cx, plain);
        let o = cx.world.get(plain).unwrap();
        assert!(
            mine_data(&cx, plain).phantom.is_none(),
            "{seek} {detonation}"
        );
        assert!(o.physical.controllers.is_empty());
    }

    // An unknown archetype makes nothing.
    assert!(
        munitions::create_mine(
            &mut cx,
            he01(),
            &launch(b"no_such_mine", 77, BASE, velocity)
        )
        .is_none()
    );
}

#[test]
fn a_spent_mine_is_destroyed_quietly_and_a_hit_one_explodes() {
    let Some((mut world, mut logic)) = setup("mine lifetime") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // Alone, with a quarter of a second to live: the update that takes
    // the lifetime below zero destroys it with `destroy(0, 0)`.
    let h = munitions::create_mine(
        &mut cx,
        he01(),
        &launch(MINE, 0, at(0.0, 0.0, 0.0), Vector::ZERO),
    )
    .unwrap();
    let mine = id(&cx, h);
    if let Some(KindData::Mine(m)) = cx.world.get_mut(h).map(|o| &mut o.data) {
        m.projectile.lifetime = 0.25;
    }
    let _ = cx.world.take_events();
    frame(&mut cx, 0.1);
    frame(&mut cx, 0.1);
    assert!(destroyed(&cx.world.take_events(), mine).is_none());
    assert!(cx.world.get(h).is_some());
    frame(&mut cx, 0.1);
    assert_eq!(destroyed(&cx.world.take_events(), mine), Some(0));
    assert!(cx.world.get(h).is_none(), "reaped");
    assert!(cx.logic.objects.get(h).is_none(), "its server entry swept");

    // A quiet removal (kind 0) next to a ship does not explode; a kill
    // (kind 1) does, inflicted by the mine's owner.
    let owner = fighter(&mut cx, at(0.0, 2000.0, 0.0));
    let owner_id = id(&cx, owner);
    let ship = fighter(&mut cx, at(0.0, 0.0, 400.0));
    let before = health(&cx, ship);
    let quiet = cx
        .world
        .create_mine(
            he01(),
            &launch(MINE, owner_id, at(0.0, 0.0, 410.0), Vector::ZERO),
        )
        .unwrap();
    world::destroy_object(&mut cx, quiet, 0, 0);
    assert_eq!(health(&cx, ship), before, "no explosion");
    assert!(cx.logic.objects.get(quiet).unwrap().destroyed);

    let killed = cx
        .world
        .create_mine(
            he01(),
            &launch(MINE, owner_id, at(0.0, 0.0, 410.0), Vector::ZERO),
        )
        .unwrap();
    let _ = cx.world.take_events();
    world::destroy_object(&mut cx, killed, 1, 12345);
    let events = cx.world.take_events();
    assert_eq!(destroyed(&events, id(&cx, killed)), Some(12345));
    assert!(health(&cx, ship) < before, "the blast reached the ship");
}

/// A phantom event for `mine`'s phantom.
fn event(cx: &Cx<'_>, mine: ObjHandle, intruder: ObjHandle, entered: bool) -> PhantomEvent {
    PhantomEvent {
        phantom: mine_data(cx, mine).phantom.expect("a phantom"),
        user_key: intruder.to_key(),
        entered,
    }
}

#[test]
fn the_phantom_counts_intruders_or_latches_a_touch() {
    let Some((mut world, mut logic)) = setup("mine phantom") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let owner = fighter(&mut cx, at(0.0, 0.0, 0.0));
    let owner_id = id(&cx, owner);
    let other = fighter(&mut cx, at(0.0, 0.0, 300.0));

    // A seeking mine counts every `CSimple` root that enters and uncounts
    // it when it leaves, even once the world has reaped the object.
    let seeking = munitions::create_mine(
        &mut cx,
        he01(),
        &launch(MINE, owner_id, at(0.0, 0.0, 100.0), Vector::ZERO),
    )
    .unwrap();
    for (who, entered) in [(owner, true), (other, true), (owner, false)] {
        let ev = event(&cx, seeking, who, entered);
        munitions::phantom_event(&mut cx, he01(), &ev);
    }
    assert_eq!(mine_state(&cx, seeking).intruder_count(), 1);
    let ev = event(&cx, seeking, other, false);
    world::destroy_object(&mut cx, other, 0, 0);
    cx.world.reap();
    assert!(cx.world.get(other).is_none());
    munitions::phantom_event(&mut cx, he01(), &ev);
    assert_eq!(mine_state(&cx, seeking).intruder_count(), 0);
    assert!(
        !mine_state(&cx, seeking).triggered,
        "a seeking mine never latches"
    );

    // A mine that does not seek latches the first touch, but not its
    // owner's while the owner-safe time runs.
    let third = fighter(&mut cx, at(0.0, 0.0, -300.0));
    let third_id = id(&cx, third);
    let plain = cx
        .world
        .create_mine(
            he01(),
            &launch(MINE, owner_id, at(0.0, 0.0, -100.0), Vector::ZERO),
        )
        .unwrap();
    if let Some(KindData::Mine(m)) = cx.world.get_mut(plain).map(|o| &mut o.data) {
        m.seek_dist = 0.0;
    }
    assert!(cx.world.attach_mine_phantom(plain, 50.0));
    let ev = event(&cx, plain, owner, true);
    munitions::phantom_event(&mut cx, he01(), &ev);
    assert!(!mine_state(&cx, plain).triggered, "the owner is safe");
    let ev = event(&cx, plain, third, true);
    munitions::phantom_event(&mut cx, he01(), &ev);
    let ev = event(&cx, plain, third, false);
    munitions::phantom_event(&mut cx, he01(), &ev);
    let state = mine_state(&cx, plain);
    assert!(state.triggered);
    assert_eq!(state.trigger_obj, third_id);
    assert_eq!(state.intruder_count(), 0, "a plain mine counts nothing");
    if let Some(KindData::Mine(m)) = cx.world.get_mut(plain).map(|o| &mut o.data) {
        m.projectile.owner_safe_time = 0.0;
    }
    let ev = event(&cx, plain, owner, true);
    munitions::phantom_event(&mut cx, he01(), &ev);
    assert_eq!(
        mine_state(&cx, plain).trigger_obj,
        owner_id,
        "no longer safe"
    );

    // With no intruders, the latched touch detonates the mine on its next
    // update: `destroy(1, toucher)`.
    let _ = cx.world.take_events();
    munitions::mine_after_update(&mut cx, plain, 0.1);
    assert_eq!(
        destroyed(&cx.world.take_events(), id(&cx, plain)),
        Some(owner_id)
    );
    // The phantom's collisions went on first.
    let p = mine_data(&cx, plain).phantom.unwrap();
    assert!(cx.world.physics(he01()).unwrap().phantom_collision_state(p));
}

#[test]
fn a_seeking_mine_spares_its_owner_while_safe_then_seeks_and_detonates() {
    let Some((mut world, mut logic)) = setup("mine seeking") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // A player in he01, so that its frames step the physics.
    world::add_player(&mut cx, he01(), 1);
    let owner = fighter(&mut cx, at(0.0, 0.0, 0.0));
    let owner_id = id(&cx, owner);
    let h = munitions::create_mine(
        &mut cx,
        he01(),
        &launch(MINE, owner_id, at(0.0, 0.0, 60.0), Vector::ZERO),
    )
    .unwrap();
    let mine = id(&cx, h);

    // The owner enters the phantom, but while the owner-safe time (4 s)
    // runs no intruder is the target: the mine does not seek.
    for _ in 0..10 {
        frame(&mut cx, 0.1);
    }
    assert_eq!(
        mine_state(&cx, h).intruder_count(),
        1,
        "the owner is inside"
    );
    let m = mine_data(&cx, h);
    assert!(!m.seeking);
    assert_eq!(m.seek_target, Vector::ZERO);
    assert_eq!(mine_state(&cx, h).seek_timer, f32::MAX, "never sought");
    let still = cx.world.get(h).unwrap().engine.position;
    assert_eq!(still, at(0.0, 0.0, 60.0));

    // Once it runs out the owner is the closest intruder, outside the
    // detonation distance: the mine seeks it (the timer starts at
    // `FLT_MAX + dt`, past 1 s, so at 0) and its controller pushes it
    // towards the owner.
    let _ = cx.world.take_events();
    let mut sought = None;
    for step in 0..60 {
        frame(&mut cx, 0.1);
        if cx.world.get(h).is_none() {
            break;
        }
        let m = mine_data(&cx, h);
        if m.seeking && sought.is_none() {
            sought = Some(step);
            assert!(mine_state(&cx, h).seek_timer < 1.0);
            let owner_pos = cx.world.get(owner).unwrap().engine.position;
            let d = (m.seek_target.z - owner_pos.z).abs();
            assert!(d < 30.0, "the target is a point of the owner's hull: {d}");
        }
    }
    let first = sought.expect("the mine sought its owner");
    assert!(
        first >= 28,
        "not before the owner-safe time ran out: {first}"
    );
    // It reached the owner and blew up, killed by it (its phantom or the
    // collision).
    let events = cx.world.take_events();
    assert_eq!(destroyed(&events, mine), Some(owner_id));
    assert!(cx.world.get(h).is_none());
}

#[test]
fn a_counter_measure_diverts_other_owners_missiles() {
    let Some((mut world, mut logic)) = setup("counter-measure") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let target = fighter(&mut cx, at(0.0, 0.0, -3000.0));
    let (mine_owner, other_owner) = (501, 502);
    let mut missiles = Vec::new();
    for (i, owner) in [
        mine_owner,
        other_owner,
        other_owner,
        other_owner,
        other_owner,
        other_owner,
    ]
    .into_iter()
    .enumerate()
    {
        let mut spawn = launch(
            MISSILE,
            owner,
            at(i as f32 * 20.0, 0.0, 0.0),
            Vector::new(0.0, 0.0, -100.0),
        );
        spawn.target = Some((target, SUBOBJ_ID_ROOT));
        missiles.push(cx.world.create_guided(he01(), &spawn).unwrap());
    }
    // The C++'s walk: the sphere's objects in order, the other owners'
    // guided munitions each roll `rand() * 100 / 0x8000` against 70.
    let cm_at = at(50.0, 0.0, 0.0);
    let hits: Vec<ObjHandle> = cx
        .world
        .physics(he01())
        .unwrap()
        .find_sphere_collisions(cm_at, 1000.0, 0x1e)
        .into_iter()
        .map(ObjHandle::from_key)
        .collect();
    let mut rng = cx.world.rng().clone();
    // The counter-measure's own spin comes first (three draws).
    let _ = csys::random_inside_sphere(0.0, &mut rng);
    let mut expected = Vec::new();
    for hit in hits {
        let Some(KindData::Guided(g)) = cx.world.get(hit).map(|o| &o.data) else {
            continue;
        };
        if g.projectile.owner_id == mine_owner {
            continue;
        }
        if rng.rand() * 100 / 0x8000 <= 70 {
            expected.push(hit);
        }
    }
    let cm = munitions::create_counter_measure(
        &mut cx,
        he01(),
        &launch(COUNTER_MEASURE, mine_owner, cm_at, Vector::ZERO),
    )
    .expect("the counter-measure is made");
    assert_eq!(*cx.world.rng(), rng, "one draw per other owner's missile");
    assert!(
        !expected.is_empty() && expected.len() < 5,
        "{}",
        expected.len()
    );
    for &m in &missiles {
        let Some(KindData::Guided(g)) = cx.world.get(m).map(|o| &o.data) else {
            panic!("a missile");
        };
        if expected.contains(&m) {
            assert_eq!((g.target, g.sub_target), (Some(cm), SUBOBJ_ID_NONE));
        } else {
            assert_eq!((g.target, g.sub_target), (Some(target), SUBOBJ_ID_ROOT));
        }
    }
    assert!(cx.logic.objects.get(cm).is_some(), "its server entry");
}

#[test]
fn set_target_refuses_without_a_seeker_and_relays_a_single_player_client() {
    let Some((mut world, mut logic)) = setup("guided targets") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let a = fighter(&mut cx, at(0.0, 0.0, -1000.0));
    let b = fighter(&mut cx, at(0.0, 0.0, 1000.0));
    let mut spawn = launch(MISSILE, 9, BASE, Vector::ZERO);
    spawn.target = Some((a, SUBOBJ_ID_ROOT));
    let missile = cx.world.create_guided(he01(), &spawn).unwrap();
    let target = |cx: &Cx<'_>| match cx.world.get(missile).map(|o| &o.data) {
        Some(KindData::Guided(g)) => (g.target, g.sub_target),
        _ => panic!("a missile"),
    };

    assert_eq!(munitions::set_target(&mut cx, missile, Some(b), 5, 0), 0);
    assert_eq!(target(&cx), (Some(b), 5));
    // A source of -1 is the server's own.
    assert_eq!(
        munitions::set_target(&mut cx, missile, None, SUBOBJ_ID_ROOT, -1),
        0
    );
    assert_eq!(target(&cx), (None, SUBOBJ_ID_ROOT));
    // In single player a client's change is only relayed.
    cx.world.set_multiplayer(false);
    assert_eq!(munitions::set_target(&mut cx, missile, Some(a), 7, 3), 0);
    assert_eq!(target(&cx), (None, SUBOBJ_ID_ROOT));
    cx.world.set_multiplayer(true);
    assert_eq!(munitions::set_target(&mut cx, missile, Some(a), 7, 3), 0);
    assert_eq!(target(&cx), (Some(a), 7));
    // A munition that does not seek refuses: -3.
    if let Some(KindData::Guided(g)) = cx.world.get_mut(missile).map(|o| &mut o.data) {
        g.seeker = 0;
    }
    assert_eq!(munitions::set_target(&mut cx, missile, Some(b), 1, 0), -3);
    assert_eq!(target(&cx), (Some(a), 7));
}

fn kill(cx: &mut Cx<'_>, h: ObjHandle, inflictor: u32) {
    let mut list = DamageList::new(1, inflictor, 0);
    list.destroyed = true;
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
}

#[test]
fn a_missile_a_counter_measure_kills_does_not_explode() {
    let Some((mut world, mut logic)) = setup_with("missile kills", |data| {
        // The shipped counter-measure has no explosion: give it the
        // mine's, so that its `destroy` can be seen.
        let mine = data.archetypes.equipment[&create_id(MINE)]
            .root
            .explosion_arch;
        if let Some(cm) = data
            .archetypes
            .equipment
            .get_mut(&create_id(COUNTER_MEASURE))
        {
            cm.root.explosion_arch = mine;
        }
    }) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let ship = fighter(&mut cx, at(0.0, 0.0, 0.0));
    let before = health(&cx, ship);
    let cm = munitions::create_counter_measure(
        &mut cx,
        he01(),
        &launch(COUNTER_MEASURE, 42, at(0.0, 3000.0, 0.0), Vector::ZERO),
    )
    .unwrap();
    let cm_id = id(&cx, cm);
    let missile = |cx: &mut Cx<'_>| {
        let spawn = launch(MISSILE, 9, at(0.0, 0.0, 12.0), Vector::ZERO);
        cx.world.create_guided(he01(), &spawn).unwrap()
    };
    // Killed by the counter-measure: no detonation.
    let m = missile(&mut cx);
    kill(&mut cx, m, cm_id);
    assert!(cx.world.get(m).unwrap().dead);
    assert_eq!(health(&cx, ship), before);
    // Killed by anything else: it explodes at its position.
    let m = missile(&mut cx);
    kill(&mut cx, m, 4242);
    assert!(health(&cx, ship) < before);

    // The counter-measure's own `destroy`: quietly with kind 0, an
    // explosion inflicted by its owner with kind 1.
    let after_missile = health(&cx, ship);
    let quiet = munitions::create_counter_measure(
        &mut cx,
        he01(),
        &launch(COUNTER_MEASURE, 42, at(0.0, 0.0, 10.0), Vector::ZERO),
    )
    .unwrap();
    world::destroy_object(&mut cx, quiet, 0, 0);
    assert_eq!(health(&cx, ship), after_missile);
    let loud = munitions::create_counter_measure(
        &mut cx,
        he01(),
        &launch(COUNTER_MEASURE, 42, at(0.0, 0.0, 10.0), Vector::ZERO),
    )
    .unwrap();
    // Any munition kills it (O46), here a gun's.
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.munition_collision_hooked(
        loud,
        4242,
        create_id(b"fc_lr_gun01_mark05_ammo"),
        SUBOBJ_ID_ROOT,
        Vector::ZERO,
        &mut hooks,
    );
    assert!(cx.world.get(loud).unwrap().dead);
    assert!(health(&cx, ship) < after_missile, "its blast");
}

#[test]
fn with_pvp_off_players_ships_do_not_damage_each_other() {
    let Some((mut world, mut logic)) = setup("pvp") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let p1 = fighter(&mut cx, at(0.0, 0.0, -2000.0));
    let p2 = fighter(&mut cx, at(0.0, 0.0, 0.0));
    let npc = fighter(&mut cx, at(0.0, 0.0, 2000.0));
    for p in [p1, p2] {
        if let Some(s) = cx.world.get_mut(p).and_then(|o| o.as_ship_mut()) {
            s.is_player = true;
        }
    }
    let (p1_id, npc_id) = (id(&cx, p1), id(&cx, npc));
    let gun = create_id(b"fc_lr_gun01_mark05_ammo");
    let shoot = |cx: &mut Cx<'_>, shooter: u32| {
        let mut hooks = Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.munition_collision_hooked(
            p2,
            shooter,
            gun,
            SUBOBJ_ID_ROOT,
            Vector::ZERO,
            &mut hooks,
        );
    };
    let hits = |events: Vec<WorldEvent>| {
        events
            .iter()
            .filter(|e| matches!(e, WorldEvent::MunitionHit { .. }))
            .count()
    };

    cx.logic.server_world.forbid_pvp = true;
    let before = health(&cx, p2);
    let _ = cx.world.take_events();
    shoot(&mut cx, p1_id);
    assert_eq!(
        hits(cx.world.take_events()),
        0,
        "a player's shot on a player"
    );
    assert_eq!(health(&cx, p2), before);
    // A mine a player owns explodes next to another player: no damage.
    let mine = cx
        .world
        .create_mine(
            he01(),
            &launch(MINE, p1_id, at(0.0, 0.0, 10.0), Vector::ZERO),
        )
        .unwrap();
    world::destroy_object(&mut cx, mine, 1, 0);
    assert_eq!(health(&cx, p2), before);
    // An NPC's shot still hits.
    shoot(&mut cx, npc_id);
    assert_eq!(hits(cx.world.take_events()), 1);
    let after_npc = health(&cx, p2);
    assert!(after_npc < before);

    // PvP on (the C++'s default): both hit.
    cx.logic.server_world.forbid_pvp = false;
    shoot(&mut cx, p1_id);
    assert_eq!(hits(cx.world.take_events()), 1);
    let mine = cx
        .world
        .create_mine(
            he01(),
            &launch(MINE, p1_id, at(0.0, 0.0, 10.0), Vector::ZERO),
        )
        .unwrap();
    let before_blast = health(&cx, p2);
    world::destroy_object(&mut cx, mine, 1, 0);
    assert!(health(&cx, p2) < before_blast);
}
