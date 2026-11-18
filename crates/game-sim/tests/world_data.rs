//! The object model on the original game data (`outdir/`, decision O15):
//! a fighter with its loadout in he01, flown, turned, fired, rammed.
//! Behaviour checks against the C++ flight model's shape, not oracle
//! comparisons (those are `scenario_replay.rs` and `controller_replay.rs`).

mod support;

use game_sim::equip::{EquipKind, types};
use game_sim::object::world::{ShipSpawn, WorldConfig, WorldEvent};
use game_sim::object::{Command, ObjectKind};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;

fn speed(v: Vector) -> f64 {
    let (x, y, z) = (f64::from(v.x), f64::from(v.y), f64::from(v.z));
    (x * x + y * y + z * z).sqrt()
}

fn fighter(
    world: &mut game_sim::object::World,
    system: u32,
    at: Vector,
) -> game_sim::object::ObjHandle {
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    world.spawn_ship(system, &spawn).expect("he_fighter spawns")
}

#[test]
fn fighter_has_its_loadout_and_flies() {
    let Some(mut world) = support::world::world(WorldConfig {
        step_mode: StepMode::SinglePlayer,
        ..WorldConfig::default()
    }) else {
        println!("outdir/ is missing: nothing to fly");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let o = world.get(ship).unwrap();
    assert_eq!(o.kind, ObjectKind::Ship);
    let eq = o.as_equipped().unwrap();
    assert!(eq.max_power > 0.0, "power plant fitted");
    assert_eq!(eq.power, eq.max_power);
    let engine = eq.equipment.first(types::ENGINE).expect("engine fitted");
    let EquipKind::Engine(state) = &engine.kind else {
        panic!("engine kind");
    };
    assert!(engine.is_active());
    assert!(state.controller_enabled);
    let guns = world.unguided_guns(ship);
    assert!(!guns.is_empty(), "guns fitted");
    println!(
        "nozzles {}, guns {}, items {}, groups {}",
        state.nozzles.len(),
        guns.len(),
        eq.equipment.len(),
        eq.arch_groups.len()
    );

    // Full throttle for three seconds: the speed rises toward the
    // terminal speed max_force / drag.
    world.command(ship, Command::SetThrottle(1.0));
    for _ in 0..90 {
        world.step(system, 1.0 / 30.0);
    }
    let i = world.inspect(ship).unwrap();
    let v = speed(i.velocity);
    println!(
        "speed after 3 s at full throttle: {v:.3} m/s, position {:?}",
        i.position
    );
    assert!(v > 10.0, "the engine pushes ({v})");
    // The ship faces -Z.
    assert!(i.velocity.z < 0.0);

    // Yaw for a second: the forward axis turns.
    world.command(ship, Command::SetAxisThrottle(Vector::new(0.0, 0.5, 0.0)));
    for _ in 0..30 {
        world.step(system, 1.0 / 30.0);
    }
    let turned = world.inspect(ship).unwrap();
    println!(
        "angular velocity after 1 s of yaw: {:?}",
        turned.angular_velocity
    );
    assert!(turned.orientation != i.orientation);
    assert!(turned.angular_velocity.y.abs() > 1e-3);
}

#[test]
fn guns_fire_bullets_that_hit_a_target() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: nothing to fire");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let shooter = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let target = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25400.0));
    let guns = world.unguided_guns(shooter);
    let mut fired = 0;
    let mut hits = 0;
    for tick in 1..=240u32 {
        if (10..=150).contains(&tick)
            && (tick - 10) % 5 == 0
            && let Some(aim) = world.inspect(target).map(|i| i.position)
        {
            world.command(
                shooter,
                Command::Fire {
                    guns: guns.clone(),
                    target: aim,
                },
            );
        }
        world.step(system, 1.0 / 30.0);
        for e in world.take_events() {
            match e {
                WorldEvent::Fired { .. } => fired += 1,
                WorldEvent::MunitionHit { .. } => hits += 1,
                _ => {}
            }
        }
    }
    let t = world.inspect(target).map(|i| (i.hit_pts, i.max_hit_pts));
    println!("bullets {fired}, hits {hits}, target {t:?}");
    assert!(fired > 0, "the guns fire");
    assert!(hits > 0, "the bullets hit");
    // Every bullet is gone once its lifetime ran out.
    assert!(
        world
            .objects_in(system)
            .iter()
            .all(|h| !world.is_projectile(*h))
    );
}

#[test]
fn destruction_is_deferred_and_handles_expire() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(0.0, 0.0, 0.0));
    let id = world.get(ship).unwrap().simple.id;
    assert_eq!(world.find(id), Some(ship));
    world.destroy(ship, 0);
    // Still readable until the reap at the end of the tick.
    assert!(world.inspect(ship).is_some_and(|i| i.dead));
    world.step(system, 1.0 / 30.0);
    assert!(world.get(ship).is_none());
    assert!(world.find(id).is_none());
    assert!(
        world
            .take_events()
            .iter()
            .any(|e| matches!(e, WorldEvent::Destroyed { id: d, .. } if *d == id))
    );
    // A new object reuses the slot under a new generation.
    let again = fighter(&mut world, system, Vector::new(0.0, 0.0, 0.0));
    assert_eq!(again.index(), ship.index());
    assert_ne!(again, ship);
}

#[test]
fn a_blast_damages_shield_then_hull() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(0.0, 0.0, 0.0));
    let blast = game_sim::object::damage::Blast {
        position: Vector::new(0.0, 0.0, 20.0),
        radius: 60.0,
        hull_damage: 400.0,
        energy_damage: 0.0,
    };
    let before = world.inspect(ship).unwrap().hit_pts;
    world.explode(system, &blast, 0, None);
    let after_first = world.inspect(ship).unwrap().hit_pts;
    // The functioning shield takes the first blast whole.
    assert_eq!(after_first, before);
    for _ in 0..10 {
        world.explode(system, &blast, 0, None);
    }
    let after = world.inspect(ship).map_or(0.0, |i| i.hit_pts);
    println!("hull {before} -> {after_first} -> {after}");
    assert!(
        after < before,
        "the hull takes damage once the shield is down"
    );
}

/// The first shield of an equipped object: its hit points.
fn shield_hp(world: &game_sim::object::World, h: game_sim::object::ObjHandle) -> f32 {
    let eq = world
        .get(h)
        .and_then(|o| o.as_equipped())
        .expect("equipped");
    eq.equipment
        .iter()
        .find_map(|e| match &e.kind {
            EquipKind::Shield(s) => Some(s.hit_pts),
            _ => None,
        })
        .expect("a shield")
}

#[test]
fn the_shield_generator_regenerates_the_shield() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(0.0, 0.0, 0.0));
    // `CEShieldGenerator::CEShieldGenerator` activates the generator.
    let eq = world.get(ship).and_then(|o| o.as_equipped()).unwrap();
    assert!(
        eq.equipment
            .iter()
            .any(|e| matches!(e.kind, EquipKind::ShieldGenerator(_)) && e.is_functioning(false))
    );
    let blast = game_sim::object::damage::Blast {
        position: Vector::new(0.0, 0.0, 20.0),
        radius: 60.0,
        hull_damage: 50.0,
        energy_damage: 0.0,
    };
    world.explode(system, &blast, 0, None);
    let hit = shield_hp(&world, ship);
    for _ in 0..31 {
        world.step(system, 1.0 / 30.0);
    }
    let later = shield_hp(&world, ship);
    println!("shield {hit} -> {later}");
    assert!(later > hit, "the damaged shield regenerates");
}

/// A gun's turret joints, as the world holds them.
fn gun_joints(
    world: &game_sim::object::World,
    h: game_sim::object::ObjHandle,
    id: game_sim::equip::SubObjId,
) -> (
    game_sim::equip::turret::JointChannel,
    game_sim::equip::turret::JointChannel,
) {
    let e = world
        .get(h)
        .and_then(|o| o.as_equipped())
        .and_then(|eq| eq.equipment.by_id(id))
        .expect("gun");
    match &e.kind {
        EquipKind::Gun(g) => (g.yaw, g.pitch),
        _ => panic!("not a gun"),
    }
}

#[test]
fn turrets_turn_onto_a_locked_target() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no turrets");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let shooter = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    // Ahead, a little up and to the right: within a gun's limits.
    let target = fighter(&mut world, system, Vector::new(15060.0, 3040.0, -25400.0));
    let guns = world.unguided_guns(shooter);
    let movable: Vec<_> = guns
        .iter()
        .copied()
        .filter(|&g| gun_joints(&world, shooter, g).0.is_bound())
        .collect();
    println!("guns {guns:?}, turret guns {movable:?}");
    assert!(!movable.is_empty(), "the fighter's guns turn on joints");
    let aim = world.inspect(target).unwrap().position;
    // The wing guns hang sideways: each pitches only outwards, so only the
    // guns on the target's side can point at it.
    let pointing: Vec<_> = movable
        .iter()
        .copied()
        .filter(|&g| world.gun_can_point_at(shooter, g, aim, 0.0))
        .collect();
    println!("guns that can point at the target {pointing:?}");
    assert!(!pointing.is_empty() && pointing.len() < movable.len());
    // Straight behind is outside every pitch window.
    let behind = Vector::new(15000.0, 3000.0, -24000.0);
    assert!(
        movable
            .iter()
            .all(|&g| !world.gun_can_point_at(shooter, g, behind, 0.0))
    );
    let movable = pointing;
    for &g in &movable {
        let (yaw, pitch) = gun_joints(&world, shooter, g);
        println!(
            "gun {g}: yaw limited {} pitch bound {}, origin {:?}",
            yaw.limited,
            pitch.is_bound(),
            world.turret_origin(shooter, g)
        );
        assert!(world.gun_set_target(shooter, g, Some(target), 0));
        assert!(!world.gun_set_target(shooter, g, Some(target), 0));
    }
    // The aim is set and the joints wait for the animation update.
    let (yaw, pitch) = gun_joints(&world, shooter, movable[0]);
    println!("aim yaw {} pitch {}", yaw.desired, pitch.desired);
    assert!(yaw.desired != 0.0 || pitch.desired != 0.0);
    assert!(!world.pending_joints().is_empty());
    for _ in 0..90 {
        world.step(system, 1.0 / 30.0);
    }
    for &g in &movable {
        let (yaw, pitch) = gun_joints(&world, shooter, g);
        println!(
            "gun {g}: yaw {} -> {}, pitch {} -> {}, settled {} {}",
            yaw.current, yaw.desired, pitch.current, pitch.desired, yaw.settled, pitch.settled
        );
        assert!(
            yaw.within(0.01) && pitch.within(0.01),
            "the joints reached the aim"
        );
        assert!(world.gun_can_see_target(shooter, g, 0.05));
    }
    // A target point overrides the lock for two seconds.
    let g = movable[0];
    world.gun_set_target_point(shooter, g, Vector::new(14940.0, 3000.0, -25400.0));
    world.step(system, 1.0 / 30.0);
    let (yaw, _) = gun_joints(&world, shooter, g);
    println!("target point yaw {}", yaw.desired);
    assert!(yaw.desired != gun_joints(&world, shooter, movable[0]).0.current || yaw.settled);
}
