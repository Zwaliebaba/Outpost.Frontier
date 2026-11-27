//! The player's flight commands on a ship of the original data (`outdir/`,
//! decision O15; work package P5.4b).
//!
//! - `throttle_clamps_and_reaches_the_manager`: `SetPlayerThrottle` clamps
//!   to [-1, 1]; the commands find the ship's behaviour manager (made on
//!   first use) and the auto flags reach its thrust manager.
//! - `the_engine_queries_read_the_ship`: the engine is active and not
//!   cruising after the spawn.
//! - `mouse_flight_engages_after_the_hold`: the hold-to-engage machine
//!   latches the origin, waits out 0.25 s, then steers to the cursor's
//!   offset from the centre; letting go resets the direction; leaving the
//!   radius forces the threshold.
//!
//! - `the_space_frame_flies_the_players_ship`: with the throttle set, the
//!   single-player space frame (object updates, the physics step) moves
//!   the player's ship forward. The step moves every body, a foreign
//!   ship's too; which objects the client updates is the module's unit
//!   test.
//!
//! Without the data each prints a notice.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_app::objects::{ClientObject, ClientObjectKind, ClientObjects};
use game_app::player_behavior::{FlightFrame, MOUSE_FLIGHT_HOLD, PlayerFlight, PlayerShip};
use game_app::space_frame;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{Command, ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A world over `outdir/` with he01 loaded and a player's fighter in it.
fn world() -> Option<(World, ObjHandle)> {
    world_with(WorldConfig::default())
}

/// The same with a world configuration.
fn world_with(world_config: WorldConfig) -> Option<(World, ObjHandle)> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    let mut world =
        World::new(Arc::new(data), world_config).with_assets(Assets::new(root.join("data")));
    let he01 = create_id(b"he01");
    world.load_system(he01);
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(15000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    spawn.owner_player = 1;
    let ship = world.spawn_ship(he01, &spawn).ok()?;
    Some((world, ship))
}

fn last_direction(world: &World, ship: ObjHandle) -> Vector {
    world
        .behavior_manager(ship)
        .expect("a manager")
        .state
        .last_direction
}

#[test]
fn throttle_clamps_and_reaches_the_manager() {
    let Some((mut world, ship)) = world() else {
        println!("notice: outdir/ has no data; nothing was flown");
        return;
    };
    let mut flight = PlayerFlight::default();
    let mut player = PlayerShip {
        world: &mut world,
        ship: Some(ship),
    };
    flight.set_throttle(&mut player, 2.5);
    assert_eq!(flight.throttle, 1.0);
    flight.set_throttle(&mut player, -3.0);
    assert_eq!(flight.throttle, -1.0);
    flight.set_throttle(&mut player, 0.0);
    assert_eq!(flight.throttle, 0.0);
    player.auto_avoidance(true);
    player.auto_level(false);
    let thrust = &world.behavior_manager(ship).expect("made").state.thrust;
    assert!(thrust.auto_avoidance && !thrust.auto_level);

    // No ship: nothing is forwarded and the last throttle reads 0.
    let mut none = PlayerShip {
        world: &mut world,
        ship: None,
    };
    flight.set_throttle(&mut none, 0.5);
    assert_eq!(flight.last_throttle(&none), 0.0);
}

#[test]
fn the_engine_queries_read_the_ship() {
    let Some((mut world, ship)) = world() else {
        println!("notice: outdir/ has no data; nothing was flown");
        return;
    };
    let player = PlayerShip {
        world: &mut world,
        ship: Some(ship),
    };
    assert!(player.engine_is_active());
    assert!(!player.engine_cruise_active());
}

#[test]
fn mouse_flight_engages_after_the_hold() {
    let Some((mut world, ship)) = world() else {
        println!("notice: outdir/ has no data; nothing was flown");
        return;
    };
    let mut flight = PlayerFlight::default();
    let mut frame = FlightFrame {
        mouse_buttons: 1,
        cursor: (768, 192),
        surface: (1024, 768),
        frame_time: 0.1,
        ..FlightFrame::default()
    };
    flight.on_raw_message(0x201);
    {
        let mut player = PlayerShip {
            world: &mut world,
            ship: Some(ship),
        };
        // The manager is made by the first command.
        player.reset_direction();
        flight.update_mouse_flight(&mut player, &frame);
        flight.update_mouse_flight(&mut player, &frame);
        assert!(!flight.engaged && !flight.hold_reached());
        assert_eq!(flight.origin, (768.0, 192.0));
        flight.update_mouse_flight(&mut player, &frame);
        assert!(flight.engaged && flight.hold_timer >= MOUSE_FLIGHT_HOLD);
    }
    assert_eq!(last_direction(&world, ship), Vector::new(0.5, -0.5, 0.0));

    // Letting go: the timer and the direction reset.
    frame.mouse_buttons = 0;
    {
        let mut player = PlayerShip {
            world: &mut world,
            ship: Some(ship),
        };
        flight.update_mouse_flight(&mut player, &frame);
    }
    assert!(!flight.engaged && flight.hold_timer == 0.0);
    assert_eq!(last_direction(&world, ship), Vector::ZERO);

    // A cursor that leaves the radius (800 / 1024 * 50 px) forces the
    // threshold before the hold is out.
    frame.mouse_buttons = 1;
    flight.on_raw_message(0x201);
    let mut player = PlayerShip {
        world: &mut world,
        ship: Some(ship),
    };
    frame.frame_time = 0.01;
    flight.update_mouse_flight(&mut player, &frame);
    frame.cursor.0 += 40;
    flight.update_mouse_flight(&mut player, &frame);
    assert_eq!(flight.hold_timer, MOUSE_FLIGHT_HOLD);
}

#[test]
fn the_space_frame_flies_the_players_ship() {
    let config = WorldConfig {
        single_player: true,
        ..WorldConfig::default()
    };
    let Some((mut world, ship)) = world_with(config) else {
        println!("notice: outdir/ has no data; nothing was flown");
        return;
    };
    let he01 = create_id(b"he01");
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(14000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    spawn.owner_player = 0;
    let other = world.spawn_ship(he01, &spawn).expect("a second fighter");
    let mut objects = ClientObjects::default();
    for h in [ship, other] {
        let id = world.get(h).unwrap().simple.id;
        objects.register(
            ClientObject {
                id,
                kind: ClientObjectKind::Ship,
                archetype: create_id(b"he_fighter"),
                handle: Some(h),
                remote_physics: false,
            },
            &world,
        );
    }
    world.command(ship, Command::SetThrottle(1.0));
    world.command(other, Command::SetThrottle(1.0));
    let start = |w: &World, h| w.get(h).unwrap().engine.position;
    let a0 = start(&world, ship);
    for _ in 0..30 {
        space_frame::simulate(&mut world, &objects, he01, 1, 1.0 / 30.0);
    }
    let a1 = start(&world, ship);
    // The fighter faces -z.
    assert!(
        a1.z < a0.z - 1.0,
        "the player's ship moved: {a0:?} -> {a1:?}"
    );
}
