use super::step::{BeamQuery, PhantomDesc};
use super::*;
use crate::physics::ivp::IvpRandom;

fn constants() -> PhySysConstants {
    PhySysConstants::default()
}

fn parms(mass: f32, name: &[u8]) -> CreateParms {
    CreateParms {
        mass,
        linear_damping: 0.0,
        angular_damping: Vector::ZERO,
        name: name.to_vec(),
        enable_collisions: true,
        inertia: None,
    }
}

fn ball(world: &mut PhysicsWorld, key: u64, radius: f32, position: Vector) -> BodyHandle {
    world.create_body(BodyDesc {
        user_key: key,
        geometry: Geometry::ball(radius),
        parms: parms(10.0, key.to_string().as_bytes()),
        position,
        orientation: Matrix::IDENTITY,
        pin_mass_center: false,
    })
}

fn step(world: &mut PhysicsWorld, host: &mut dyn ControllerHost, random: &mut IvpRandom, dt: f32) {
    world.clear_collisions();
    world.simulate_dtime(dt, host, random);
}

struct Thrust(Vector);

impl ControllerHost for Thrust {
    fn do_simulation_controller(&mut self, key: u64, context: &mut ControllerContext<'_>) {
        assert_eq!(key, 99);
        let dt = context.event.delta_time;
        assert!(context.core().mass > 0.0);
        context.push(self.0, dt);
        context.rotate(Vector::new(0.0, 0.0, 1.0), dt);
    }
}

#[test]
fn bodies_start_frozen_and_wake_on_push() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let h = ball(&mut world, 1, 1.0, Vector::new(5.0, 0.0, 0.0));
    assert!(world.core(h).unwrap().is_frozen());
    assert_eq!(world.moving_object_count(), 0);
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    assert_eq!(world.transform(h).unwrap().0, Vector::new(5.0, 0.0, 0.0));
    world.add_to_velocity(h, Vector::new(10.0, 0.0, 0.0));
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    assert_eq!(world.moving_objects(), &[h]);
    assert_eq!(world.velocity(h, true), Vector::new(10.0, 0.0, 0.0));
    // The body sits at the PSI start; by the next PSI it is 0.5 m on.
    let (p, _) = world.transform(h).unwrap();
    assert!((p.x - 5.5).abs() < 1e-5, "{p:?}");
    assert_eq!(world.user_key(h), Some(1));
    assert!(world.contains(h));
    assert_eq!(world.mass(h), Some(10.0));
    assert_eq!(world.moment_of_inertia(h), Some(Vector::new(4.0, 4.0, 4.0)));
}

#[test]
fn controllers_push_through_the_hook() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let h = ball(&mut world, 1, 1.0, Vector::ZERO);
    let c = world
        .register_controller(h, 99, CONTROLLER_PRIORITY)
        .unwrap();
    let mut host = Thrust(Vector::new(100.0, 0.0, 0.0));
    for _ in 0..10 {
        step(&mut world, &mut host, &mut random, 0.1);
    }
    // 100 N on 10 kg for 1 s.
    let v = world.velocity(h, true);
    assert!((v.x - 10.0).abs() < 1e-4, "{v:?}");
    assert!(world.angular_velocity_os(h).z > 0.0);
    world.unregister_controller(c);
    let before = world.velocity(h, true);
    step(&mut world, &mut host, &mut random, 0.1);
    assert_eq!(world.velocity(h, true), before);
}

#[test]
fn controller_buckets_follow_registration_and_rebuilds() {
    let mut world = PhysicsWorld::new(1, &constants());
    let h = ball(&mut world, 1, 1.0, Vector::ZERO);
    let a = world
        .register_controller(h, 1, CONTROLLER_PRIORITY)
        .unwrap();
    world
        .register_controller(h, 2, CONTROLLER_PRIORITY)
        .unwrap();
    world
        .register_controller(h, 3, CONTROLLER_PRIORITY)
        .unwrap();
    // The last registered runs first.
    assert_eq!(world.controller_run_order(h), vec![3, 2, 1]);
    // A rebuilt unit runs them in registration order.
    world.rebuild_controller_buckets(h);
    assert_eq!(world.controller_run_order(h), vec![1, 2, 3]);
    // A controller registered again joins the buckets at the end.
    world.unregister_controller(a);
    world
        .register_controller(h, 4, CONTROLLER_PRIORITY)
        .unwrap();
    assert_eq!(world.controller_run_order(h), vec![4, 2, 3]);
    world.rebuild_controller_buckets(h);
    assert_eq!(world.controller_run_order(h), vec![2, 3, 4]);
}

#[test]
fn damping_and_sleep() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let h = ball(&mut world, 1, 1.0, Vector::ZERO);
    world.set_damping(h, 1.0, Vector::new(1.0, 1.0, 1.0));
    assert_eq!(world.damping(h), Some((1.0, Vector::new(1.0, 1.0, 1.0))));
    world.add_to_velocity(h, Vector::new(1.0, 0.0, 0.0));
    for _ in 0..200 {
        step(&mut world, &mut NoControllers, &mut random, 0.1);
    }
    assert!(
        world.core(h).unwrap().is_frozen(),
        "an idle body goes to sleep"
    );
    assert_eq!(world.moving_object_count(), 0);
    assert_eq!(world.velocity(h, true), Vector::ZERO);
}

#[test]
fn head_on_balls_bounce_with_the_material_elasticity() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let a = ball(&mut world, 1, 1.0, Vector::new(-3.0, 0.0, 0.0));
    let b = ball(&mut world, 2, 1.0, Vector::new(3.0, 0.0, 0.0));
    world.add_to_velocity(a, Vector::new(20.0, 0.0, 0.0));
    world.add_to_velocity(b, Vector::new(-20.0, 0.0, 0.0));
    let mut events = Vec::new();
    for _ in 0..10 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
        events.extend_from_slice(world.collisions());
    }
    assert_eq!(events.len(), 1, "{events:?}");
    let event = events[0];
    assert_eq!((event.object_a, event.object_b), (1, 2));
    assert!(event.surface_normal.x < -0.99, "the normal points to A");
    assert!((event.relative_velocity.x - 40.0).abs() < 1e-3);
    let (va, vb) = (world.velocity(a, true), world.velocity(b, true));
    assert!(
        (va.x + 18.0).abs() < 1e-3 && (vb.x - 18.0).abs() < 1e-3,
        "{va:?} {vb:?}"
    );
    let damage = crate::physics::CollisionDamage::default();
    assert_eq!(
        damage.calc_collision_damage(1, event.relative_velocity, event.surface_normal),
        0.0
    );
}

#[test]
fn same_group_passes_through_and_filters_apply() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let a = ball(&mut world, 1, 1.0, Vector::new(-3.0, 0.0, 0.0));
    let b = ball(&mut world, 2, 1.0, Vector::new(3.0, 0.0, 0.0));
    world.set_collision_group(a, b"fleet_long_name");
    world.set_collision_group(b, b"fleet_long_other");
    assert_eq!(world.collision_group(a), b"fleet_l".to_vec());
    world.add_to_velocity(a, Vector::new(40.0, 0.0, 0.0));
    for _ in 0..5 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
        assert!(world.collisions().is_empty());
    }
    assert!(world.velocity(a, true).x > 39.0);
    world.set_collision_group(b, b"");
    world.set_collision_filter(Some(|_, _| false));
    world.set_collision_state(a, false);
    assert!(!world.collision_state(a));
}

#[test]
fn fast_small_bodies_do_not_tunnel() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let wall = world.create_body(BodyDesc {
        user_key: 1,
        geometry: Geometry::cuboid(
            Vector::new(-0.05, -10.0, -10.0),
            Vector::new(0.05, 10.0, 10.0),
        ),
        parms: parms(1.0e6, b"1"),
        position: Vector::new(10.0, 0.0, 0.0),
        orientation: Matrix::IDENTITY,
        pin_mass_center: true,
    });
    let bullet = ball(&mut world, 2, 0.05, Vector::ZERO);
    world.set_single_contact(bullet, true);
    world.add_to_velocity(bullet, Vector::new(900.0, 0.0, 0.0));
    let mut hit = false;
    for _ in 0..3 {
        step(&mut world, &mut NoControllers, &mut random, 0.0999);
        hit |= !world.collisions().is_empty();
    }
    assert!(hit);
    assert!(
        !world.collision_state(bullet),
        "a projectile leaves after its first contact"
    );
    assert!(world.transform(bullet).unwrap().0.x < 10.0);
    assert!(world.contains(wall));
}

#[test]
fn rays_spheres_and_detached_parts() {
    let mut world = PhysicsWorld::new(1, &constants());
    let near = ball(&mut world, 1, 1.0, Vector::new(10.0, 0.0, 0.0));
    let far = ball(&mut world, 2, 1.0, Vector::new(20.0, 0.0, 0.0));
    let hits = world.find_ray_collisions(Vector::ZERO, Vector::new(100.0, 0.0, 0.0), 8);
    assert_eq!(
        hits.iter().map(|h| h.user_key).collect::<Vec<_>>(),
        vec![1, 2]
    );
    assert!((hits[0].point.x - 9.0).abs() < 1e-4);
    assert!(hits[0].normal.x < -0.99);
    assert_eq!(
        world
            .find_ray_collisions(Vector::ZERO, Vector::new(100.0, 0.0, 0.0), 1)
            .len(),
        1
    );
    let only = world.find_ray_intersect(far, Vector::ZERO, Vector::new(100.0, 0.0, 0.0), 4);
    assert_eq!(only.len(), 1);
    assert_eq!(only[0].body, far);
    world.set_part_attached(near, 0, false);
    assert_eq!(
        world.find_ray_collisions(Vector::ZERO, Vector::new(100.0, 0.0, 0.0), 8)[0].user_key,
        2
    );
    world.set_part_attached(near, 0, true);
    assert_eq!(
        world.find_sphere_collisions(Vector::new(11.5, 0.0, 0.0), 1.0, 8),
        vec![1]
    );
    assert_eq!(
        world.find_sphere_collisions(Vector::new(15.0, 0.0, 0.0), 5.5, 1),
        vec![1]
    );
    assert!(
        world
            .find_sphere_collisions(Vector::new(15.0, 0.0, 0.0), 1.0, 8)
            .is_empty()
    );
}

#[test]
fn phantoms_report_enter_and_leave() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let ship = ball(&mut world, 1, 1.0, Vector::new(-10.0, 0.0, 0.0));
    let p = world.create_phantom(PhantomDesc {
        geometry: Geometry::ball(3.0),
        position: Vector::ZERO,
        orientation: Matrix::IDENTITY,
        name: Vec::new(),
        listener: true,
    });
    assert!(world.phantom_collision_state(p));
    world.add_to_velocity(ship, Vector::new(50.0, 0.0, 0.0));
    let mut events = Vec::new();
    for _ in 0..8 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
        events.extend(world.take_phantom_events());
    }
    let entered: Vec<bool> = events.iter().map(|e| e.entered).collect();
    assert_eq!(entered, vec![true, false], "{events:?}");
    assert_eq!(events[0].user_key, 1);
    assert!(world.phantom_intruders(p).is_empty());
    // A phantom attached to the ship travels with it.
    let carried = world.create_phantom(PhantomDesc {
        geometry: Geometry::cuboid(Vector::new(-1.0, -1.0, -1.0), Vector::new(1.0, 1.0, 1.0)),
        position: Vector::new(0.0, 5.0, 0.0),
        orientation: Matrix::IDENTITY,
        name: Vec::new(),
        listener: false,
    });
    assert!(world.attach_phantom(carried, ship));
    let other = ball(&mut world, 3, 0.5, Vector::ZERO);
    let ship_x = world.transform(ship).unwrap().0.x;
    world.beam(
        other,
        Vector::new(ship_x, 5.0, 0.0),
        &Matrix::IDENTITY,
        false,
    );
    step(&mut world, &mut NoControllers, &mut random, 0.01);
    assert_eq!(world.phantom_intruders(carried), vec![3]);
    let closest = world.closest_phantom_intruder(carried, |_| true).unwrap();
    assert_eq!(closest.0, 3);
    assert!(world.take_phantom_events().is_empty(), "no listener");
    world.set_phantom_collision_state(carried, false);
    step(&mut world, &mut NoControllers, &mut random, 0.01);
    assert!(world.phantom_intruders(carried).is_empty());
    assert!(world.detach_phantom(carried));
    world.destroy_phantom(carried);
    assert!(!world.phantom_collision_state(carried));
}

#[test]
fn beams_hit_the_nearest_body_outside_their_group() {
    let mut world = PhysicsWorld::new(1, &constants());
    let own = ball(&mut world, 1, 1.0, Vector::new(5.0, 0.0, 0.0));
    world.set_collision_group(own, b"team");
    let _target = ball(&mut world, 2, 1.0, Vector::new(10.0, 0.0, 0.0));
    let event = world
        .generate_beam_collision(&BeamQuery {
            user_key: 7,
            start: Vector::ZERO,
            direction: Vector::new(1.0, 0.0, 0.0),
            length: 50.0,
            collision_group: b"team".to_vec(),
            velocity: Vector::new(600.0, 0.0, 0.0),
        })
        .unwrap();
    assert_eq!((event.object_a, event.object_b), (2, 7));
    assert!((event.point.x - 9.0).abs() < 1e-4);
    assert_eq!(event.relative_velocity, Vector::new(600.0, 0.0, 0.0));
    assert_eq!(world.collisions().len(), 1);
}

#[test]
fn attach_and_detach_rebuild_the_compound() {
    let mut world = PhysicsWorld::new(1, &constants());
    let mut random = IvpRandom::default();
    let parent = ball(&mut world, 1, 2.0, Vector::ZERO);
    let child = ball(&mut world, 2, 0.5, Vector::new(0.0, 3.0, 0.0));
    world.attach(parent, child);
    assert_eq!(world.mass(child), Some(10.0));
    world.add_to_velocity(parent, Vector::new(10.0, 0.0, 0.0));
    for _ in 0..4 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
    }
    let (pp, _) = world.transform(parent).unwrap();
    let (cp, _) = world.transform(child).unwrap();
    assert!(
        (cp.x - pp.x).abs() < 1e-4 && (cp.y - 3.0).abs() < 1e-4,
        "{pp:?} {cp:?}"
    );
    assert_eq!(world.moving_object_count(), 2);
    let detached = world
        .detach(
            child,
            &DetachParms {
                linear_damping: 0.0,
                angular_damping: Vector::ZERO,
                name: Vec::new(),
                mass: 1.0,
                inertia: None,
            },
        )
        .unwrap();
    assert!(!world.contains(child));
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    assert_eq!(world.mass(detached), Some(1.0));
    assert!((world.velocity(detached, true).x - 10.0).abs() < 1e-4);
    world.destroy_body(detached);
    assert_eq!(world.moving_objects(), &[parent]);
}

#[test]
fn golems_hold_and_follow_their_targets() {
    let c = constants();
    let mut world = PhysicsWorld::new(1, &c);
    let mut random = IvpRandom::default();
    let part = ball(&mut world, 1, 1.0, Vector::ZERO);
    world.attach_golem(part, &GolemParms::for_part(&c));
    let now = world.current_time();
    world.golem_set_target_position(part, Vector::new(1.0, 0.0, 0.0), Vector::ZERO, now);
    world.golem_set_target_quaternion(part, Quaternion::IDENTITY, now, None, 0.0);
    for _ in 0..30 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
    }
    let (p, _) = world.transform(part).unwrap();
    assert!((p.x - 1.0).abs() < 0.05, "{p:?}");
    // A target beyond GOLEM_MAX_DELTA_POSITION is reached by beaming.
    let now = world.current_time();
    world.golem_set_target_position(part, Vector::new(100.0, 0.0, 0.0), Vector::ZERO, now);
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    assert!((world.transform(part).unwrap().0.x - 100.0).abs() < 1e-3);
}

#[test]
fn remote_correction_follows_the_samples() {
    let c = constants();
    let mut world = PhysicsWorld::new(1, &c);
    let mut random = IvpRandom::default();
    let remote = ball(&mut world, 1, 1.0, Vector::ZERO);
    world.rc_begin(
        remote,
        &c,
        Vector::ZERO,
        Vector::new(10.0, 0.0, 0.0),
        Quaternion::IDENTITY,
        Vector::ZERO,
    );
    world.rc_set(
        remote,
        Vector::new(1.0, 0.0, 0.0),
        Quaternion::IDENTITY,
        0.1,
    );
    assert!(world.remote_simulation(remote).is_some());
    for _ in 0..5 {
        step(&mut world, &mut NoControllers, &mut random, 0.05);
    }
    let (p, _) = world.transform(remote).unwrap();
    assert!(p.x > 1.0, "{p:?}");
    assert_eq!(
        world.velocity(remote, false),
        world.remote_simulation(remote).unwrap().velocity
    );
    world.rc_end(remote);
    assert!(world.remote_simulation(remote).is_none());
}

#[test]
fn facade_setters() {
    let mut world = PhysicsWorld::new(3, &constants());
    let mut random = IvpRandom::default();
    let h = ball(&mut world, 1, 1.0, Vector::ZERO);
    assert_eq!(world.system_id(), 3);
    world.set_mass(h, 20.0);
    assert_eq!(world.mass(h), Some(20.0));
    assert_eq!(world.moment_of_inertia(h), Some(Vector::new(4.0, 4.0, 4.0)));
    world.set_moment_of_inertia(h, Vector::new(1.0, 2.0, 3.0));
    world.linear_impulse(h, Vector::new(1.0, 0.0, 0.0), 20.0);
    world.angular_impulse(h, Vector::new(0.0, 0.0, 3.0), 1.0);
    world.angular_impulse_os(h, Vector::new(1.0, 0.0, 0.0), 1.0);
    world.add_to_angular_velocity_os(h, Vector::new(0.0, 1.0, 0.0));
    world.add_impulse_at_point(h, Vector::new(0.0, 20.0, 0.0), Vector::new(1.0, 0.0, 0.0));
    step(&mut world, &mut NoControllers, &mut random, 0.05);
    let v = world.velocity(h, true);
    assert!(
        (v.x - 1.0).abs() < 1e-6 && (v.y - 1.0).abs() < 1e-6,
        "{v:?}"
    );
    let w = world.angular_velocity_os(h);
    assert!(w.x > 0.0 && w.y > 0.0 && w.z > 0.0, "{w:?}");
    assert!(world.center_of_mass(h).is_some());
    world.deactivate(h);
    assert_eq!(world.moving_object_count(), 0);
    world.destroy_body(h);
    assert!(!world.contains(h));
}

#[test]
fn bodies_from_compact_surfaces_and_the_default_part() {
    let bytes = include_bytes!("../../../tests/fixtures/oracle/sur/multi.sur");
    let loaded = crate::sur::load(bytes);
    let mut world = PhysicsWorld::new(1, &constants());
    for part in loaded.parts.values() {
        let Some(Ok(surface)) = part.compact_surface() else {
            continue;
        };
        let geometry = Geometry::from_surface(&surface);
        let h = world.create_body(BodyDesc {
            user_key: 5,
            geometry,
            parms: parms(100.0, b"5"),
            position: Vector::ZERO,
            orientation: Matrix::IDENTITY,
            pin_mass_center: false,
        });
        assert!(world.core(h).unwrap().upper_limit_radius >= 0.0);
    }
    let h = world.create_body(BodyDesc {
        user_key: 6,
        geometry: Geometry::default_part(),
        parms: CreateParms {
            inertia: Some(Vector::new(1.0, 2.0, 3.0)),
            ..parms(2.0, b"6")
        },
        position: Vector::new(1.0, 2.0, 3.0),
        orientation: Matrix::IDENTITY,
        pin_mass_center: false,
    });
    assert_eq!(world.moment_of_inertia(h), Some(Vector::new(1.0, 2.0, 3.0)));
    assert_eq!(world.transform(h).unwrap().0, Vector::new(1.0, 2.0, 3.0));
    assert_eq!(world.center_of_mass(h), Some(Vector::new(1.0, 2.0, 3.25)));
    let tiny = world.create_body(BodyDesc {
        user_key: 7,
        geometry: Geometry::default_part(),
        parms: parms(0.0, b"7"),
        position: Vector::ZERO,
        orientation: Matrix::IDENTITY,
        pin_mass_center: true,
    });
    assert_eq!(
        world.mass(tiny),
        Some(1.0),
        "a mass below the epsilon becomes 1"
    );
}

#[test]
fn the_game_filter_reads_the_client_classes() {
    let run = |class_a: u32| {
        let mut world = PhysicsWorld::new(1, &constants());
        let mut random = IvpRandom::default();
        let a = ball(&mut world, 1, 1.0, Vector::new(-3.0, 0.0, 0.0));
        let b = ball(&mut world, 2, 1.0, Vector::new(3.0, 0.0, 0.0));
        world.set_client_class(a, class_a);
        world.set_client_class(b, 0x503);
        world.set_collision_filter(Some(|a, b| a & 0x27 == 0x27 || b & 0x27 == 0x27));
        world.add_to_velocity(a, Vector::new(40.0, 0.0, 0.0));
        let mut events = 0;
        for _ in 0..10 {
            step(&mut world, &mut NoControllers, &mut random, 0.05);
            events += world.collisions().len();
        }
        events
    };
    assert_eq!(run(0x503), 0, "two ships pass through each other");
    assert_eq!(run(0xa7), 1, "a missile hits the ship");
}
