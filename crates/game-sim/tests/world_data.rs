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
            {
                world.command(
                    shooter,
                    Command::Fire {
                        guns: guns.clone(),
                        target: aim,
                    },
                );
            }
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
