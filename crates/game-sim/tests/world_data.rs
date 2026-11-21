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

#[test]
fn scanner_lists_and_tractor_pulls() {
    use game_sim::equip::misc::TractorFailure;
    use game_sim::object::world::{EquipStatus, LootSpawn, equipment_val};
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no sensors");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let other = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25400.0));
    let far = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -45000.0));
    let loot = world
        .spawn_loot(
            system,
            &LootSpawn {
                archetype: create_id(b"ge_s_cm_01_ammo_crate"),
                contents: create_id(b"ge_s_cm_01_ammo"),
                units: 5,
                contents_hit_pts: 10.0,
                owner: 0,
                position: Vector::new(15000.0, 3000.0, -25060.0),
                velocity: Vector::ZERO,
                ..LootSpawn::default()
            },
        )
        .expect("loot spawns");
    for _ in 0..15 {
        world.step(system, 1.0 / 30.0);
    }
    let seen = world.scan_list(ship);
    println!("scan list: {} objects", seen.len());
    assert!(seen.contains(&other), "the near fighter is listed");
    assert!(!seen.contains(&ship), "never the owner");
    assert!(
        !seen.contains(&far),
        "the far fighter is out of radar range"
    );

    // Equipment status of a gun: active, powered, ready.
    let gun = world.unguided_guns(ship)[0];
    let status = world.equipment_status(ship, gun).expect("a gun");
    println!("gun status {status:?}");
    assert_eq!(status.flags & EquipStatus::ACTIVE, EquipStatus::ACTIVE);
    assert_eq!(
        status.equip_flags & (EquipStatus::POWER_OK | EquipStatus::REFIRE_READY),
        EquipStatus::POWER_OK | EquipStatus::REFIRE_READY
    );
    assert!(matches!(
        world.equipment_val(ship, gun, equipment_val::MUZZLE_VELOCITY, 0.0),
        Ok(game_sim::object::world::EquipmentVal::Float(v)) if v > 0.0
    ));
    assert_eq!(world.equipment_status(ship, 0x7777), Err(-2));

    // The tractor reaches for the loot, holds it and asks to consume it.
    let tractor = world
        .get(ship)
        .and_then(|o| o.as_equipped())
        .and_then(|eq| eq.equipment.first(types::TRACTOR))
        .map(|e| e.id)
        .expect("a tractor");
    assert_eq!(
        world.tractor_add_target(ship, tractor, loot),
        TractorFailure::Ok
    );
    assert_eq!(
        world.tractor_add_target(ship, tractor, far),
        TractorFailure::NoContents
    );
    world.take_events();
    let mut reached = None;
    let mut consumed = None;
    for tick in 1..=120u32 {
        world.step(system, 1.0 / 30.0);
        for ev in world.take_events() {
            if let WorldEvent::Equip { event, .. } = ev {
                match event {
                    game_sim::equip::EquipEvent::TractorReached(_) => {
                        reached = reached.or(Some(tick))
                    }
                    game_sim::equip::EquipEvent::TractorConsume(_) => {
                        consumed = consumed.or(Some(tick))
                    }
                    _ => {}
                }
            }
        }
    }
    println!("tractor reached at {reached:?}, consume at {consumed:?}");
    let (r, c) = (
        reached.expect("the tip reaches the loot"),
        consumed.expect("consumed"),
    );
    assert!(c > r + 25, "the tip holds the loot about a second");
}

#[test]
fn a_gate_opens_and_closes_and_a_bay_cycles() {
    use game_sim::object::KindData;
    use game_sim::object::anim::BayState;
    use game_sim::object::world::SolarSpawn;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no gates");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let gate = world
        .spawn_solar(
            system,
            &SolarSpawn {
                archetype: create_id(b"jumpgate"),
                loadout: 0,
                nickname: create_id(b"test_gate"),
                position: Vector::new(0.0, 0.0, 0.0),
                orientation: Matrix::IDENTITY,
                // A gate plays its open animation (`CSolar::init`'s
                // destination test).
                dest_system: create_id(b"he02"),
                ..SolarSpawn::default()
            },
        )
        .expect("the gate spawns");
    let duration = match &world.get(gate).unwrap().data {
        KindData::Solar(s) => s.gate_anim.map(|p| p.duration),
        _ => None,
    };
    println!("gate animation {duration:?} s");
    assert!(duration.is_some_and(|d| d > 0.0), "Sc_open dock is found");
    world.set_always_update(gate, true);
    assert_eq!(world.activate_gate(gate, true), Some(true));
    let mut codes = Vec::new();
    let dt = 1.0 / 30.0;
    for tick in 1..=800u32 {
        if tick == 400 {
            assert_eq!(world.activate_gate(gate, false), Some(true));
        }
        world.step(system, dt);
        for ev in world.take_events() {
            if let WorldEvent::Gate { code, .. } = ev {
                codes.push((tick, code));
            }
        }
    }
    println!("gate notifications {codes:?}");
    let order: Vec<i32> = codes.iter().map(|c| c.1).collect();
    assert_eq!(order, vec![0, 1, 2, 3]);
    // Open 10 s after it starts to open, closing 5 s after the request.
    assert!((300..=303).contains(&(codes[1].0 - codes[0].0)));
    assert!((150..=152).contains(&(codes[2].0 - 400)));
    // Closed once the reverse playback ran its 5.83 s.
    assert!((174..=177).contains(&(codes[3].0 - codes[2].0)));

    // A ship's bay: the doors take the bay animation's time each way.
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let bay =
        |w: &game_sim::object::World| w.get(ship).and_then(|o| o.as_ship()).map(|s| s.bay_state);
    let time = world.open_bay(ship);
    println!("bay doors need {time} s");
    assert_eq!(bay(&world), Some(BayState::Opening));
    let mut ticks = 0;
    while bay(&world) != Some(BayState::Open) && ticks < 600 {
        world.step(system, dt);
        ticks += 1;
    }
    println!("bay open after {ticks} ticks");
    assert_eq!(bay(&world), Some(BayState::Open));
    assert!((f64::from(ticks) * f64::from(dt) - time).abs() < 0.1);
    world.close_bay(ship);
    assert_eq!(bay(&world), Some(BayState::Closing));
    for _ in 0..=ticks + 2 {
        world.step(system, dt);
    }
    assert_eq!(bay(&world), Some(BayState::Closed));
}

#[test]
fn hardpoint_summary_and_explosion_sphere() {
    use game_sim::object::bounds::HardpointSummary;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no hardpoints");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(100.0, 200.0, 300.0));
    let root = world.get(ship).unwrap().engine.instance;
    let mut summary = HardpointSummary::default();
    let count = summary.analyze_instance(world.scene(), root);
    println!("{count} hardpoints");
    assert!(count > 10);
    assert!(summary.find_name(b"hpweapon01").is_some(), "case ignored");
    assert_eq!(summary.find_name_instance(b"HpWeapon01"), root);
    assert!(summary.info_by_name(b"NoSuchHardpoint").is_none());
    summary.expire_instance();
    assert_eq!(summary.find_name_instance(b"HpWeapon01"), None);
    let (_, ps) = world
        .find_hardpoint_ps(root, b"HpWeapon01")
        .expect("on the root");
    println!("HpWeapon01 at {:?}", ps.translation);

    let (radius, center) = world.explosion_dmg_bounding_sphere(ship).expect("a sphere");
    println!("explosion sphere {radius} at {center:?}");
    assert!(radius > 1.0 && radius < 100.0);
    let d = speed(Vector::new(
        center.x - 100.0,
        center.y - 200.0,
        center.z - 300.0,
    ));
    assert!(d < f64::from(radius), "the sphere holds the ship's origin");
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

#[test]
fn an_ai_fighter_gets_a_manager_and_idles() {
    use game_sim::ai::behavior::{DirectiveOp, ids};
    use game_sim::ai::state_graph::GraphType;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: nothing to fly");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    world.command(ship, Command::SetThrottle(1.0));
    let graph = world
        .data()
        .state_graphs
        .find(b"FIGHTER", GraphType::Leader);
    assert!(graph >= 0, "FIGHTER graph");
    assert_eq!(world.submit_state(ship, graph, None), 0);
    let manager = world.behavior_manager(ship).expect("a manager");
    assert_eq!(manager.state.state_graph_id(), graph);
    assert_eq!(manager.state.behavior, ids::IDLE);
    let eq = world.get(ship).and_then(|o| o.as_equipped()).unwrap();
    assert!(eq.has_behavior);
    assert_eq!(eq.behavior_index, ids::IDLE);
    for _ in 0..30 {
        world.step(system, 1.0 / 30.0);
    }
    // The idle behaviour stops the ship.
    let i = world.inspect(ship).unwrap();
    assert_eq!(i.ship.map(|s| s.throttle), Some(0.0));
    // A delay directive runs and ends.
    assert_eq!(world.submit_directive(ship, &DirectiveOp::Delay(0.5)), 1);
    assert_eq!(
        world.behavior_manager(ship).unwrap().state.behavior,
        ids::DELAY
    );
    for _ in 0..30 {
        world.step(system, 1.0 / 30.0);
    }
    let state = &world.behavior_manager(ship).unwrap().state;
    assert_eq!(state.directive[0], 0);
    assert!(state.completed.contains(&(ids::DELAY, 0)));
}

#[test]
fn the_ship_leads_its_target_over_its_gun_statistics() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no gun statistics");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let shooter = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let target = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25400.0));
    // Three guns without a seeker; the missile launcher, off until the AI
    // switches it on, is not counted.
    let stats = world.ship_gun_stats(shooter).expect("a ship");
    println!("gun stats {stats:?}");
    assert_eq!(stats.active_guns, 3);
    assert_eq!(stats.cm_droppers, 0);
    assert!(stats.avg_muzzle_velocity > 0.0 && stats.max_munition_range > 400.0);
    // Switching it on dirties the cache; it counts as a dropper then.
    let unguided = world.unguided_guns(shooter);
    let launcher = game_sim::ai::world::AiWorld::find_equipment(&world, shooter, types::GUN)
        .unwrap_or_default()
        .into_iter()
        .find(|id| !unguided.contains(id))
        .expect("the missile launcher");
    world.command(shooter, Command::ActivateEquip(launcher, true));
    assert_eq!(
        world.ship_gun_stats(shooter).map(|s| s.cm_droppers),
        Some(1)
    );
    assert_eq!(world.ship_lead_fire_position(shooter), None, "no target");
    world.command(shooter, Command::SetTarget(Some(target), 0));
    // Both at rest: the lead is the target's centre of mass, 400 m away.
    let lead = world.ship_lead_fire_position(shooter).expect("in range");
    assert!((f64::from(lead.z) + 25400.0).abs() < 10.0, "{lead:?}");
}

#[test]
fn the_ai_reads_loot_and_the_hold_and_points_the_tractor() {
    use game_sim::ai::behavior::ids;
    use game_sim::ai::loot::LootBehavior;
    use game_sim::ai::world::{AiCommand, AiWorld};
    use game_sim::object::world::LootSpawn;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no loot");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let loot = world
        .spawn_loot(
            system,
            &LootSpawn {
                archetype: create_id(b"ge_s_cm_01_ammo_crate"),
                contents: create_id(b"ge_s_cm_01_ammo"),
                units: 5,
                contents_hit_pts: 10.0,
                owner: 0,
                position: Vector::new(15000.0, 3000.0, -25060.0),
                velocity: Vector::ZERO,
                ..LootSpawn::default()
            },
        )
        .expect("loot spawns");
    let view = AiWorld::loot(&world, loot).expect("a container");
    println!("loot {view:?}");
    assert_eq!(view.units, 5);
    assert!(view.can_ai_tractor);
    // The contents are a countermeasure.
    assert_eq!(view.contents_class, 0x21);
    assert!(AiWorld::loot(&world, ship).is_none());
    let (remaining, hold) = AiWorld::cargo_hold(&world, ship).expect("a ship's hold");
    println!("hold {remaining} of {hold}");
    assert!(hold > 0.0 && remaining >= 0.0 && remaining <= f64::from(hold));
    assert!(AiWorld::cargo_hold(&world, loot).is_none());

    // The manager's loot behaviour takes the tractor's reach, the system
    // manager its id; the tractor command takes the loot.
    assert!(world.ensure_behavior_manager(ship));
    let m = world.behavior_manager(ship).expect("a manager");
    let tractor = m.state.sys.tractor.equip_id;
    assert_ne!(tractor, 0);
    let b = m
        .behavior(ids::LOOT)
        .and_then(|b| b.as_any())
        .and_then(|a| a.downcast_ref::<LootBehavior>())
        .expect("the loot behaviour");
    assert_eq!(b.has_tractor, 1);
    let reach = b.tractor_reach;
    let range = AiWorld::equipment(&world, ship, tractor)
        .and_then(|i| i.range)
        .expect("a range");
    assert_eq!(reach, (f64::from(range) * 0.75) as f32);
    let tractor_at = |equipment| AiCommand::Tractor {
        equipment,
        target: loot,
    };
    assert_eq!(AiWorld::command(&mut world, ship, tractor_at(tractor)), 0);
    assert_eq!(AiWorld::command(&mut world, ship, tractor_at(0x7777)), -2);
}

/// The live field rocks of a system.
fn rocks(world: &game_sim::object::World, system: u32) -> Vec<game_sim::object::ObjHandle> {
    world
        .objects_in(system)
        .into_iter()
        .filter(|h| {
            world
                .get(*h)
                .is_some_and(|o| o.kind == ObjectKind::Asteroid && !o.dead)
        })
        .collect()
}

#[test]
fn an_asteroid_field_fills_around_a_ship_and_empties_behind_it() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no asteroid fields");
        return;
    };
    let system = create_id(b"ve03");
    let count = world.load_asteroid_fields(system);
    println!("ve03: {count} asteroid fields");
    assert_eq!(count, 5, "the five [Asteroids] sections of br03.ini");
    // `ram_asteroid`'s start, in the Keswick ice field.
    let pos = Vector::new(53637.0, 0.0, -25391.0);
    let zone_of = |w: &game_sim::object::World, i: usize| {
        let f = &w.asteroid_fields(system)[i];
        f.field
            .zone
            .and_then(|z| w.data().universe.zones.get(&z))
            .cloned()
    };
    let near: Vec<usize> = (0..count)
        .filter(|&i| world.asteroid_fields(system)[i].near_field(zone_of(&world, i).as_ref(), pos))
        .collect();
    println!("fields near the ship: {near:?}");
    assert!(!near.is_empty(), "the ship is in a field");
    assert!(rocks(&world, system).is_empty());
    let ship = fighter(&mut world, system, pos);
    world.step(system, 1.0 / 30.0);
    let cubes: usize = near
        .iter()
        .map(|&i| world.asteroid_fields(system)[i].cubes.len())
        .sum();
    let placed = rocks(&world, system);
    println!("after one step: {cubes} cubes, {} rocks", placed.len());
    assert!(cubes > 0 && cubes <= 9 * near.len());
    assert!(!placed.is_empty(), "the filled cubes place rocks");
    // Every rock is the shared object of its cube, fixed and listed.
    for &h in &placed {
        let o = world.get(h).expect("a rock");
        let game_sim::object::KindData::Asteroid(a) = &o.data else {
            panic!("an asteroid");
        };
        assert!(a.listed && !a.mine);
        assert!(world.body(h).is_some());
        assert_eq!(world.find(o.simple.id), Some(h));
    }
    // The cubes stay while the ship is there.
    for _ in 0..60 {
        world.step(system, 1.0 / 30.0);
    }
    assert_eq!(rocks(&world, system).len(), placed.len());
    // Without an observer they count 3 s, go far, and go at the next walk.
    world.destroy(ship, 0);
    let mut gone_at = None;
    for tick in 1..=200u32 {
        world.step(system, 1.0 / 30.0);
        if gone_at.is_none() && rocks(&world, system).is_empty() {
            gone_at = Some(tick);
        }
    }
    println!("the rocks went after {gone_at:?} steps");
    let gone_at = gone_at.expect("the rocks are dropped");
    assert!((90..=93).contains(&gone_at), "3 s, then a walk");
    assert!(
        near.iter()
            .all(|&i| world.asteroid_fields(system)[i].cubes.is_empty())
    );
}

#[test]
fn a_docking_point_opens_and_closes() {
    use game_sim::object::world::ActivateResult;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no docking points");
        return;
    };
    let system = create_id(b"he01");
    let station = create_id(b"he01_11");
    let made = world.load_system_solars_where(system, |s| s.nickname == station);
    let h = *made.first().expect("he01_11 spawns");
    let points: Vec<(Vec<u8>, Option<f32>)> = world
        .get(h)
        .and_then(|o| o.as_equipped())
        .map(|eq| {
            eq.docking_points
                .iter()
                .map(|p| (p.anim.clone(), p.playback.map(|b| b.duration)))
                .collect()
        })
        .unwrap_or_default();
    println!("docking points {points:?}");
    let index = points
        .iter()
        .position(|p| p.1.is_some_and(|d| d > 0.0))
        .expect("a docking point with an animation");
    let duration = points[index].1.unwrap_or(0.0);
    assert_eq!(
        world.activate_docking_point(h, true, index),
        ActivateResult::Changed
    );
    assert_eq!(
        world.activate_docking_point(h, true, index),
        ActivateResult::NoChange
    );
    assert_eq!(
        world.activate_docking_point(h, true, 99),
        ActivateResult::Failed
    );
    let (states, any) = world.docking_activate_state(h);
    assert!(any && states[index]);
    // A solar far from players updates only while kept updated.
    world.step(system, 1.0 / 30.0);
    assert!(
        world
            .take_events()
            .iter()
            .all(|e| !matches!(e, WorldEvent::DockAnim { .. }))
    );
    world.set_always_update(h, true);
    let time = |w: &game_sim::object::World| {
        w.get(h)
            .and_then(|o| o.as_equipped())
            .and_then(|eq| eq.docking_points[index].playback)
            .map_or(-1.0, |p| p.time)
    };
    let dt = 1.0 / 30.0;
    let mut events = Vec::new();
    let mut open_at = None;
    for tick in 1..=600u32 {
        if tick == 300 {
            assert_eq!(
                world.activate_docking_point(h, false, index),
                ActivateResult::Changed
            );
        }
        world.step(system, dt);
        for ev in world.take_events() {
            if let WorldEvent::DockAnim { index: i, open, .. } = ev {
                events.push((tick, i, open));
            }
        }
        if open_at.is_none() && time(&world) >= duration {
            open_at = Some(tick);
        }
    }
    println!("dock animation {duration} s: events {events:?}, open at {open_at:?}");
    assert_eq!(events.first(), Some(&(1, index, true)));
    let open_at = open_at.expect("the point opens");
    let expected = (f64::from(duration) / f64::from(dt)).ceil() as u32;
    assert!(
        open_at.abs_diff(expected + 1) <= 1,
        "opens over the animation"
    );
    assert_eq!(events.get(1).map(|e| (e.0, e.2)), Some((300, false)));
    assert_eq!(time(&world), 0.0, "closed again");
}

#[test]
fn cargo_capacities_and_the_tractor_refusals() {
    use game_sim::equip::Equip;
    use game_sim::equip::misc::TractorFailure;
    use game_sim::object::world::{LootSpawn, ScanResult};
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no cargo");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let ship = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let hold = world.cargo_hold_size(ship).expect("a hold");
    let free = world.cargo_hold_remaining(ship).expect("room");
    println!("hold {free} of {hold}");
    assert_eq!(hold, 25.0);
    assert_eq!(world.repair_kit_capacity_remaining(ship), Some(12));
    assert_eq!(world.shield_battery_capacity_remaining(ship), Some(12));
    let ammo = create_id(b"ge_s_cm_01_ammo");
    let max_ammo = world.data().constants.general.max_player_ammo;
    let carried = |w: &game_sim::object::World| {
        w.get(ship).and_then(|o| o.as_equipped()).and_then(|eq| {
            eq.equipment
                .iter()
                .find(|e| e.arch_id == ammo)
                .and_then(|e| match &e.kind {
                    EquipKind::Cargo(c) => Some(c.count.cast_signed()),
                    _ => None,
                })
        })
    };
    let have = carried(&world).unwrap_or(0);
    assert_eq!(
        world.ammo_capacity_remaining(ship, ammo),
        Some(if have > max_ammo { 0 } else { max_ammo - have })
    );
    assert_eq!(world.ammo_capacity_remaining(ship, 0), Some(-1));
    let gold = create_id(b"commodity_gold");
    let gold_space = world.space_for_cargo_type(ship, gold).expect("a ship");
    assert_eq!(gold_space, free.floor() as i32, "gold takes 1 per unit");
    let spawn = |w: &mut game_sim::object::World, contents: &[u8]| {
        w.spawn_loot(
            system,
            &LootSpawn {
                archetype: create_id(b"ge_s_cm_01_ammo_crate"),
                contents: create_id(contents),
                units: 5,
                contents_hit_pts: 10.0,
                owner: 0,
                position: Vector::new(15000.0, 3000.0, -25060.0),
                velocity: Vector::ZERO,
                ..LootSpawn::default()
            },
        )
        .expect("loot spawns")
    };
    let tractor = world
        .get(ship)
        .and_then(|o| o.as_equipped())
        .and_then(|eq| eq.equipment.first(types::TRACTOR))
        .map(|e| e.id)
        .expect("a tractor");
    let ammo_loot = spawn(&mut world, b"ge_s_cm_01_ammo");
    let gold_loot = spawn(&mut world, b"commodity_gold");
    assert_eq!(
        world.tractor_verify_target(ship, tractor, Some(ammo_loot)),
        TractorFailure::Ok
    );
    assert_eq!(
        world.tractor_verify_target(ship, tractor, Some(gold_loot)),
        TractorFailure::Ok
    );
    // A full stack of countermeasures: no room for more ammunition.
    let add = |w: &mut game_sim::object::World, id: u16, arch: u32, count: u32| {
        let a = w
            .data()
            .archetypes
            .equipment
            .get(&arch)
            .cloned()
            .expect("an archetype");
        let item = Equip::cargo(id, &a, false, 1.0, count);
        w.get_mut(ship)
            .and_then(|o| o.as_equipped_mut())
            .expect("equipped")
            .equipment
            .add(item);
    };
    if have == 0 {
        add(&mut world, 0x4000, ammo, max_ammo.cast_unsigned());
    } else if let Some(eq) = world.get_mut(ship).and_then(|o| o.as_equipped_mut()) {
        for e in eq.equipment.iter_mut() {
            if let (true, EquipKind::Cargo(c)) = (e.arch_id == ammo, &mut e.kind) {
                c.count = max_ammo.cast_unsigned();
            }
        }
    }
    assert_eq!(world.ammo_capacity_remaining(ship, ammo), Some(0));
    assert_eq!(
        world.tractor_verify_target(ship, tractor, Some(ammo_loot)),
        TractorFailure::AmmoFull
    );
    // Gold to the brim: the hold is full.
    let left = world.cargo_hold_remaining(ship).expect("room");
    add(&mut world, 0x4001, gold, left.ceil() as u32);
    assert_eq!(world.cargo_hold_remaining(ship), Some(0.0));
    assert_eq!(world.space_for_cargo_type(ship, gold), Some(0));
    assert_eq!(
        world.tractor_verify_target(ship, tractor, Some(gold_loot)),
        TractorFailure::CargoFull
    );

    // The cargo scan of another ship lists its equipment and cargo.
    let other = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25200.0));
    let scanner = world
        .get(ship)
        .and_then(|o| o.as_equipped())
        .and_then(|eq| eq.equipment.first(types::SCANNER))
        .map(|e| e.id)
        .expect("a scanner");
    let (result, list) = world.scan_cargo(ship, scanner, Some(other));
    println!("cargo scan {result:?}: {} items", list.len());
    assert_eq!(result, ScanResult::Complete);
    let items = world
        .get(other)
        .and_then(|o| o.as_equipped())
        .map_or(0, |eq| eq.equipment.traverse(u32::MAX, false).len());
    assert!(!list.is_empty() && list.len() <= items);
    assert!(list.iter().any(|e| e.equipped && e.hardpoint.is_some()));
    assert_eq!(
        world.scan_cargo(ship, scanner, None).0,
        ScanResult::NoTarget
    );
    let far = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -45000.0));
    assert_eq!(
        world.scan_cargo(ship, scanner, Some(far)).0,
        ScanResult::TooFar
    );
}

#[test]
fn wingmen_hold_their_slots_and_a_follower_follows() {
    use game_sim::ai::behavior::{DirectiveOp, FollowOp, GotoOp, ids};
    use game_sim::ai::state_graph::GraphType;
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: nothing to fly");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let leader = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -25000.0));
    let wing = fighter(&mut world, system, Vector::new(14955.0, 3000.0, -24986.0));
    let follower = fighter(&mut world, system, Vector::new(15000.0, 3000.0, -24700.0));
    let graphs = &world.data().state_graphs;
    let lead_graph = graphs.find(b"FIGHTER", GraphType::Leader);
    let escort_graph = graphs.find(b"FIGHTER", GraphType::Escort);
    assert!(escort_graph >= 0 && escort_graph != lead_graph);
    world.submit_state(leader, lead_graph, None);
    world.submit_state(wing, escort_graph, None);
    world.submit_state(follower, lead_graph, None);
    // The leader leads itself, then the wingman joins at its slot.
    assert_eq!(
        world.update_formation_state(leader, Some(leader), Vector::ZERO),
        0
    );
    let offset = Vector::new(-45.0, 0.0, 14.0);
    assert_eq!(world.update_formation_state(wing, Some(leader), offset), 0);
    let lv = world.ai_view(leader).unwrap();
    let wv = world.ai_view(wing).unwrap();
    assert_eq!(lv.formation_followers, vec![wv.id]);
    assert_eq!(wv.formation_leader, Some(lv.id));
    assert_eq!(wv.formation_offset, Some(offset));
    let goto = GotoOp {
        kind: 1,
        destination: Vector::new(15000.0, 3000.0, -28000.0),
        range: 100.0,
        no_cruise: 1,
        ..GotoOp::default()
    };
    assert_eq!(world.submit_directive(leader, &DirectiveOp::Goto(goto)), 1);
    let follow = FollowOp {
        target: lv.id,
        offset: Vector::new(60.0, 0.0, 120.0),
        ..FollowOp::default()
    };
    assert_eq!(
        world.submit_directive(follower, &DirectiveOp::Follow(follow)),
        1
    );
    for _ in 0..600 {
        world.step(system, 1.0 / 30.0);
    }
    let behavior = |h| world.behavior_manager(h).unwrap().state.behavior;
    assert_eq!(behavior(wing), ids::FORMATION);
    assert_eq!(behavior(follower), ids::FOLLOW);
    let (lp, wp, fp) = (
        world.inspect(leader).unwrap().position,
        world.inspect(wing).unwrap().position,
        world.inspect(follower).unwrap().position,
    );
    // The leader has flown; the wingman keeps near its slot and the
    // follower near its offset (both in the leader's frame, facing -Z).
    assert!(f64::from(lp.z) < -26000.0, "leader at {lp:?}");
    let near = |p: Vector, x: f64, z: f64| {
        let dx = f64::from(p.x) - (f64::from(lp.x) + x);
        let dz = f64::from(p.z) - (f64::from(lp.z) + z);
        (dx * dx + dz * dz).sqrt() < 150.0
    };
    assert!(near(wp, -45.0, 14.0), "wingman at {wp:?}, leader {lp:?}");
    assert!(near(fp, 60.0, 120.0), "follower at {fp:?}, leader {lp:?}");
    assert_eq!(world.ai_view(follower).unwrap().follow_leader, Some(lv.id));
}

#[test]
fn a_turret_joint_takes_the_narrower_hardpoint_limits() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no cruiser");
        return;
    };
    let system = create_id(b"he01");
    world.load_system(system);
    let spawn = ShipSpawn::new(
        create_id(b"li_cruiser"),
        create_id(b"li_n_li_cruiser"),
        Vector::new(15000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    let cruiser = world.spawn_ship(system, &spawn).expect("li_cruiser spawns");
    // `ConnectAttachedHardpoints`: the ship's turret hardpoint turns 160
    // degrees either way, the turret's own connector a full turn; the
    // joint keeps the narrower range (the oracle's engine limits).
    let turret = world.turret_geometry(cruiser, 40).expect("a turret");
    println!("yaw {:?} pitch {:?}", turret.yaw, turret.pitch);
    assert_eq!(turret.yaw, Some((-2.792_529_8, 2.792_52)));
    assert_eq!(turret.pitch, Some((0.0, 1.570_779_9)));
}
