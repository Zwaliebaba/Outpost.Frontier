//! The server's object state (record 0007 point 1) over the original data
//! (`outdir/`): the invincibility reduction, marks, a player-protected
//! ship whose hull stops at its allowed loss under gun fire, the
//! destruction of a player's current ship (`SpaceExit`, the slot's ship
//! cleared) and a relative-health set outside a damage callback.

use std::cell::RefCell;
use std::path::Path;
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::objects::{Invincibility, ServerObject};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::SUBOBJ_ID_ROOT;
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn command(kind: u32, protect: bool, from_players: bool, loss: f32) -> Invincibility {
    Invincibility {
        kind,
        protect,
        from_players,
        max_hp_loss: loss,
    }
}

#[test]
fn invincibility_commands_reduce_to_one_state() {
    let mut o = ServerObject::default();
    assert_eq!(o.invincible_timer, 1.0);
    o.set_invincible(command(1, true, false, 0.5));
    o.set_invincible(command(2, false, true, 0.25));
    assert!(o.invincible && o.invincible_flag);
    assert_eq!(o.invincible_timer, 0.25);
    // The same kind replaces its command and is not counted again.
    o.set_invincible(command(2, false, false, 0.75));
    assert!(o.invincible, "kind 1 still protects");
    assert!(!o.invincible_flag);
    assert_eq!(o.invincible_timer, 0.5, "kind 1's loss is the smaller");
    assert_eq!(o.invincibilities.len(), 2);
    assert!(o.is_damage_blocked(5, 0));
    assert!(!o.is_damage_blocked(5, 1), "players are not blocked now");
    assert!(!o.is_damage_blocked(0x10, 0), "causes from 0x10 never are");
    o.mark_for_player(3, 1);
    o.mark_for_player(3, 1);
    assert_eq!(o.marked_by, [3]);
    assert!(o.is_marked_by(3));
    o.mark_for_player(3, 0);
    assert!(!o.is_marked_by(3));
}

fn fighter(cx: &mut Cx<'_>, system: u32, owner: u32, z: f32) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(15000.0, 3000.0, z),
        Matrix::IDENTITY,
    );
    spawn.owner_player = owner;
    cx.world
        .spawn_ship(system, &spawn)
        .expect("he_fighter spawns")
}

fn shots(cx: &mut Cx<'_>, target: ObjHandle, shooter: u32, count: usize) {
    let munition = create_id(b"fc_lr_gun01_mark05_ammo");
    for _ in 0..count {
        let mut hooks = Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.munition_collision_hooked(
            target,
            shooter,
            munition,
            SUBOBJ_ID_ROOT,
            Vector::ZERO,
            &mut hooks,
        );
    }
}

#[test]
fn server_state_on_the_damage_and_destruction_paths() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no server objects");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::SpaceExit { system, player } => {
                format!("SpaceExit {system} {player}")
            }
            ControllerMessage::ObjectDestroyed(e) => format!(
                "ObjectDestroyed {} {} kind {} drops {}",
                e.id,
                e.killer,
                e.kind,
                e.dropped_equip.len()
            ),
            ControllerMessage::LootSpawned { source, count, .. } => {
                format!("LootSpawned {source} {count}")
            }
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    cx.world.load_system(he01);
    let shooter = fighter(&mut cx, he01, 0, -24000.0);
    let shooter_id = cx.world.get(shooter).unwrap().simple.id;

    // An unprotected NPC is destroyed by enough shots: its death-fuse
    // notice, `ObjectDestroyed` with what it drops, its explosion, and a
    // loot container for each dropped item (at most six).
    let plain = fighter(&mut cx, he01, 0, -25000.0);
    let plain_id = cx.world.get(plain).unwrap().simple.id;
    let _ = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    let before = cx.world.objects_in(he01).len();
    shots(&mut cx, plain, shooter_id, 200);
    assert!(cx.world.get(plain).unwrap().dead, "200 shots destroy it");
    let heard = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    // Every hit's entries send `ObjectDamaged` first.
    let first = heard.iter().find(|l| *l != "ObjectDamaged");
    assert_eq!(first.map(String::as_str), Some("DeathFuseLit"), "{heard:?}");
    let destroyed = heard
        .iter()
        .find(|l| l.starts_with("ObjectDestroyed"))
        .expect("ObjectDestroyed");
    assert!(destroyed.starts_with(&format!("ObjectDestroyed {plain_id} {shooter_id} kind 1")));
    let drops: usize = destroyed.rsplit(' ').next().unwrap().parse().unwrap();
    let spawned = heard
        .iter()
        .filter(|l| l.starts_with("LootSpawned"))
        .count();
    assert!(drops > 0, "a fighter drops lootable equipment");
    assert!(
        spawned >= drops.min(6) && spawned <= 6,
        "{spawned} containers for {drops} items"
    );
    let loot = cx
        .world
        .objects_in(he01)
        .into_iter()
        .filter(|&h| {
            cx.world
                .get(h)
                .is_some_and(|o| o.kind == game_sim::object::ObjectKind::Loot)
        })
        .count();
    assert_eq!(loot, spawned);
    assert_eq!(cx.world.objects_in(he01).len(), before + spawned);
    assert!(cx.logic.objects.get(plain).unwrap().destroyed, "the latch");
    assert_eq!(cx.world.get(plain).unwrap().simple.hit_pts, 0.0);

    // A ship protected from NPCs loses at most a quarter of its hull.
    let guarded = fighter(&mut cx, he01, 0, -26000.0);
    cx.logic
        .objects
        .entry(cx.world, guarded)
        .unwrap()
        .set_invincible(command(1, true, false, 0.25));
    shots(&mut cx, guarded, shooter_id, 200);
    let g = cx.world.get(guarded).unwrap();
    assert!(!g.dead, "blocked damage does not destroy");
    let relative = g.simple.hit_pts / g.simple.max_hit_pts;
    assert!(
        (relative - 0.75).abs() < 1e-4,
        "the hull stops at 75 %: {relative}"
    );
    assert!(!cx.logic.objects.get(guarded).unwrap().in_damage_callback);

    // A relative-health set outside a callback applies at once.
    let pending = game_logic::objects::set_relative_health(&mut cx, guarded, 0.5).expect("now");
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world
        .apply_damage_list_hooked(guarded, &pending, &mut hooks);
    let g = cx.world.get(guarded).unwrap();
    assert_eq!(g.simple.hit_pts, g.simple.max_hit_pts * 0.5);

    // A player's current ship destroyed by damage: health 0, ship cleared,
    // `SpaceExit` to the bus.
    let own = fighter(&mut cx, he01, 1, -27000.0);
    let own_id = cx.world.get(own).unwrap().simple.id;
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01;
        slot.ship_object_id = own_id;
        slot.relative_health = 1.0;
    }
    let _ = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    world::destroy_object(&mut cx, own, 1, shooter_id);
    assert!(cx.world.get(own).unwrap().dead);
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!((slot.ship_object_id, slot.relative_health), (0, 0.0));
    let heard = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    // `ObjectDestroyed` first; a multiplayer player's ship drops no loot.
    assert_eq!(heard.len(), 2, "{heard:?}");
    assert!(heard[0].starts_with(&format!("ObjectDestroyed {own_id} {shooter_id} kind 1")));
    assert_eq!(heard[1], format!("SpaceExit {he01} 1"));
    // Every session hears the player's death (group notice 0x14).

    // The reap drops the entries of reaped objects.
    let before = cx.logic.objects.len();
    cx.world.reap();
    cx.logic.objects.sweep(cx.world);
    assert!(cx.logic.objects.len() < before);
}

#[test]
fn loot_containers_decay_unseen_and_lose_units() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no loot");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    cx.world.load_system(he01);
    let boron = create_id(b"commodity_boron");
    let crate_arch = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&boron)
        .and_then(|a| match &a.kind {
            game_sim::data::archetype::equipment::EquipmentKind::Commodity {
                loot_appearance,
                ..
            } => *loot_appearance,
            _ => None,
        })
        .unwrap_or(boron);
    let spawn = |x: f32| game_sim::object::world::LootSpawn {
        archetype: crate_arch,
        contents: boron,
        units: 3,
        contents_hit_pts: 10.0,
        position: Vector::new(x, 0.0, -25000.0),
        ..game_sim::object::world::LootSpawn::default()
    };
    let lonely = game_logic::objects::create_loot(&mut cx, he01, 0, &[spawn(0.0)])[0];
    let watched = game_logic::objects::create_loot(&mut cx, he01, 0, &[spawn(60000.0)])[0];
    // A player's ship next to the second container.
    let guard = fighter(&mut cx, he01, 1, -25000.0);
    if let Some(o) = cx.world.get_mut(guard) {
        o.engine.position = Vector::new(60000.0, 100.0, -25000.0);
    }

    // Contents damage: a non-root entry at 0 takes a unit.
    let mut list = game_sim::object::damage::DamageList::new(1, 0, 0);
    list.add_damage_entry(
        game_sim::equip::SUBOBJ_ID_POWER,
        0.0,
        game_sim::equip::Fate::None,
    );
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(lonely, &list, &mut hooks);
    match &cx.world.get(lonely).unwrap().data {
        game_sim::object::KindData::Loot(l) => assert_eq!(l.units, 2),
        _ => panic!("loot"),
    }

    // 70 s with nobody near the first: it goes, the watched one stays.
    for _ in 0..70 {
        world::step_system(&mut cx, he01, 1.0);
    }
    assert!(
        cx.world.get(lonely).is_none_or(|o| o.dead),
        "unseen loot is removed"
    );
    assert!(
        cx.world.get(watched).is_some_and(|o| !o.dead),
        "a player keeps it"
    );
}

#[test]
fn an_indestructible_solar_keeps_its_hull() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no solars");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        HEARD.with(|h| h.borrow_mut().push(message.name().to_owned()));
    });
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let solars = cx.world.load_system_solars(he01);
    let station = *solars
        .iter()
        .find(|&&h| {
            cx.world.get(h).is_some_and(|o| {
                o.simple.max_hit_pts > 0.0
                    && matches!(&o.data, game_sim::object::KindData::Solar(s) if !s.destructible)
            })
        })
        .expect("an indestructible solar with hit points");
    let before = cx.world.get(station).unwrap().simple.hit_pts;
    let _ = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    shots(&mut cx, station, 0, 5);
    assert_eq!(cx.world.get(station).unwrap().simple.hit_pts, before);
    let heard = HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()));
    assert_eq!(
        heard.iter().filter(|l| *l == "ObjectDamaged").count(),
        5,
        "{heard:?}"
    );
    // The bare world has no such rule.
    cx.world.munition_collision(
        station,
        0,
        create_id(b"fc_lr_gun01_mark05_ammo"),
        SUBOBJ_ID_ROOT,
        Vector::ZERO,
    );
    assert!(cx.world.get(station).unwrap().simple.hit_pts < before);
}
