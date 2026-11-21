//! Moving a ship between systems and unloading a system (decision record
//! 0007, points 5 and 6) on the original game data: the ship leaves he01
//! with its physics parameters, is in transit with no body and no list,
//! is still found by id, arrives in he02 with the same parameters and its
//! controllers, flies there, and he01's unload removes its solars and its
//! physics world while the ship stays.

mod support;

use game_sim::object::world::{ServerHooks, ShipSpawn, WorldConfig};
use game_sim::object::{Command, ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};

fn fighter(world: &mut World, system: u32) -> ObjHandle {
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(15000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    world.spawn_ship(system, &spawn).expect("he_fighter spawns")
}

#[test]
fn a_ship_leaves_one_system_and_enters_another() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no transfer");
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    world.load_system(he01);
    let solars = world.load_system_solars(he01).len();
    assert!(solars > 0, "he01 has solars");
    let ship = fighter(&mut world, he01);
    let id = world.get(ship).unwrap().simple.id;
    let (_, body) = world.body(ship).unwrap();
    let physics = world.physics(he01).unwrap();
    let mass = physics.mass(body).unwrap();
    let collisions = physics.collision_state(body);
    let controllers = world.get(ship).unwrap().physical.controllers.len();
    assert!(controllers >= 4, "steering, nudge, strafe, the ship");

    // Leaving: parameters saved, no body, no list, found by id.
    let parms = world.leave_system(ship).expect("leaves");
    assert_eq!(parms.mass, mass);
    assert_eq!(parms.enable_collisions, collisions);
    assert!(parms.inertia.is_some());
    assert!(world.in_transit(ship));
    assert!(world.body(ship).is_none());
    assert!(world.get(ship).unwrap().physical.controllers.is_empty());
    assert_eq!(world.find(id), Some(ship));
    assert!(!world.objects_in(he01).contains(&ship));
    assert!(world.leave_system(ship).is_none(), "already in transit");
    assert!(
        !world.enter_system(
            ship,
            create_id(b"no_such_system"),
            Vector::ZERO,
            Matrix::IDENTITY
        ),
        "an unloaded system takes nothing"
    );
    assert!(world.in_transit(ship));

    // Arriving in he02.
    world.load_system(he02);
    let at = Vector::new(-30000.0, 0.0, -25000.0);
    assert!(world.enter_system(ship, he02, at, Matrix::IDENTITY));
    assert!(!world.in_transit(ship));
    assert_eq!(world.objects_in(he02), vec![ship]);
    let (system, body) = world.body(ship).unwrap();
    assert_eq!(system, he02);
    let physics = world.physics(he02).unwrap();
    assert_eq!(physics.mass(body), Some(mass));
    assert_eq!(physics.collision_state(body), collisions);
    assert_eq!(physics.collision_group(body), parms.name);
    assert_eq!(
        world.get(ship).unwrap().physical.controllers.len(),
        controllers
    );
    assert_eq!(world.get(ship).unwrap().engine.position, at);

    // It flies in he02.
    world.command(ship, Command::SetThrottle(1.0));
    for _ in 0..30 {
        world.step(he02, 1.0 / 30.0);
    }
    let moved = world.inspect(ship).unwrap().position;
    assert_ne!(moved, at, "the engine pushes in the new system");

    // Unloading he01 removes its solars and its physics; the ship stays.
    assert_eq!(world.unload_system(he01), solars);
    assert!(!world.system_loaded(he01));
    assert!(world.physics(he01).is_none());
    assert!(world.get(ship).is_some());
    assert_eq!(world.unload_system(he01), 0);
}

/// Counts the hook calls of a frame and turns the physics off.
#[derive(Default)]
struct Counting {
    updates: usize,
    after: usize,
    reaps: usize,
}

impl ServerHooks for Counting {
    fn object_update(&mut self, _world: &mut World, _object: ObjHandle, _dt: f32) -> bool {
        self.updates += 1;
        true
    }

    fn object_after_update(&mut self, _world: &mut World, _object: ObjHandle, _dt: f32) {
        self.after += 1;
    }

    fn after_reap(&mut self, _world: &mut World, _system: u32) {
        self.reaps += 1;
    }

    fn steps_physics(&mut self, _world: &World, _system: u32) -> bool {
        false
    }
}

#[test]
fn the_server_hooks_see_each_sub_step() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no hooks");
        return;
    };
    let he01 = create_id(b"he01");
    world.load_system(he01);
    let ship = fighter(&mut world, he01);
    world.command(ship, Command::SetThrottle(1.0));
    let before = world.inspect(ship).unwrap().position;
    let mut hooks = Counting::default();
    // 0.5 s outside single player: 0.2 + 0.2 + 0.1.
    assert_eq!(world.step_with(he01, 0.5, &mut hooks), 3);
    assert_eq!(hooks.reaps, 3);
    assert_eq!(hooks.updates, 3, "the one ship, once per sub-step");
    assert_eq!(hooks.after, 3, "after each own update");
    assert_eq!(
        world.inspect(ship).unwrap().position,
        before,
        "no physics without players"
    );
}
