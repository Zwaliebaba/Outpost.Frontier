//! The server equipment's fire, launches, jettison and cargo decay over the
//! original data (`outdir/`): `EqObj::slot_34` with `SrvGun::fire` and
//! `launch_bullets`, a missile launcher's `launch_guided` through
//! `StarSystem::create_guided`, the mine and counter-measure droppers'
//! `fire_forward` and launches through `create_mine` and
//! `create_counter_measure`, a client's `fire_unchecked`, the
//! counter-measure dropper's and the starved thruster's notices,
//! `Ship::slot_38`, `tick_jettison` and `compute_jettison_launch`, and
//! `EqObj::decay_cargo` with `apply_equip_entry`'s stack rule. Expected
//! values follow the C++ (`Server/server_gameobject.cpp`,
//! `server_classes.cpp`).

use std::path::Path;
use std::sync::Arc;

use game_logic::server::{Cx, Logic, RecordingSink};
use game_logic::world::{self, Hooks};
use game_logic::{cargo, equip_fire, ship};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipEvent, EquipKind, Fate, SUBOBJ_ID_ROOT, SubObjId, types};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, NoServerHooks, ShipSpawn, WorldConfig};
use game_sim::object::{KindData, ObjHandle, ObjectKind, World, ship::BayState};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

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

fn setup_config(what: &str, config: WorldConfig) -> Option<(World, Logic)> {
    let data = data(what)?;
    let mut world = World::new(data, config).with_assets(Assets::new(outdir().join("data")));
    world.load_system(he01());
    let logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    Some((world, logic))
}

fn setup(what: &str) -> Option<(World, Logic)> {
    setup_config(what, WorldConfig::default())
}

fn he01() -> u32 {
    create_id(b"he01")
}

const BASE: Vector = Vector::new(15000.0, 3000.0, 0.0);

/// The cargo stack the tests add: its sub-object id, unused by the loadout.
const CARGO_ID: SubObjId = 0x400;
const LUXURY_FOOD: &[u8] = b"commodity_luxury_food";

/// A bounty hunter's light fighter (missile launcher, mine dropper,
/// counter-measure dropper, three guns, a thruster), with `cargo` units of
/// luxury food, and its server equipment.
fn fighter(cx: &mut Cx<'_>, position: Vector, cargo: i32) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"MSN04_Bounty_Hunter_Helion_Light_Fighter"),
        position,
        Matrix::IDENTITY,
    );
    if cargo > 0 {
        let mut desc = EquipDesc::new(cx.world.strings());
        desc.id = CARGO_ID;
        desc.arch_id = create_id(LUXURY_FOOD);
        desc.count = cargo;
        spawn.equipment.push(desc);
    }
    let h = cx
        .world
        .spawn_ship(he01(), &spawn)
        .expect("he_fighter spawns");
    ship::rebuild_server_equip(cx, h);
    h
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).map_or(0, |o| o.simple.id)
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// The first item of the type bits whose projectile seeks (or not).
fn item(cx: &Cx<'_>, h: ObjHandle, mask: u32, seeking: Option<bool>) -> SubObjId {
    let eq = cx.world.get(h).and_then(|o| o.equipped.as_deref()).unwrap();
    eq.equipment
        .traverse(mask, false)
        .into_iter()
        .filter_map(|s| eq.equipment.get(s))
        .find(|e| {
            seeking.is_none_or(|want| {
                cx.world
                    .launcher_projectile(h, e.id)
                    .is_some_and(|p| (p.seeker != 0) == want)
            })
        })
        .map(|e| e.id)
        .expect("the item")
}

/// The refire delay passed (the clock past any delay).
fn ready(cx: &mut Cx<'_>, h: ObjHandle, equip: SubObjId) {
    let e = cx
        .world
        .get_mut(h)
        .and_then(|o| o.equipped.as_deref_mut())
        .and_then(|eq| eq.equipment.by_id_mut(equip))
        .unwrap();
    match &mut e.kind {
        EquipKind::Gun(g) => g.launcher.refire_elapsed = 100.0,
        EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => l.refire_elapsed = 100.0,
        _ => panic!("not a launcher"),
    }
}

fn refire_elapsed(cx: &Cx<'_>, h: ObjHandle, equip: SubObjId) -> f32 {
    let e = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.by_id(equip))
        .unwrap();
    match &e.kind {
        EquipKind::Gun(g) => g.launcher.refire_elapsed,
        EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => l.refire_elapsed,
        _ => panic!("not a launcher"),
    }
}

fn fire_count(cx: &Cx<'_>, h: ObjHandle, equip: SubObjId) -> u32 {
    cx.logic
        .objects
        .get(h)
        .and_then(|o| o.server_equip.iter().find(|s| s.equip == equip))
        .and_then(|s| s.launcher())
        .map_or(0, |l| l.fire_count)
}

/// The count of the first cargo stack of `arch`.
fn stack(cx: &Cx<'_>, h: ObjHandle, arch: u32) -> u32 {
    let eq = cx.world.get(h).and_then(|o| o.equipped.as_deref()).unwrap();
    eq.equipment
        .traverse(types::CARGO, true)
        .into_iter()
        .filter_map(|s| eq.equipment.get(s))
        .find(|c| c.arch_id == arch)
        .map_or(0, |c| match &c.kind {
            EquipKind::Cargo(c) => c.count,
            _ => 0,
        })
}

fn of_kind(cx: &Cx<'_>, kind: ObjectKind) -> Vec<ObjHandle> {
    cx.world
        .objects_in(he01())
        .into_iter()
        .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == kind))
        .collect()
}

/// `fire_forward`'s aim: the barrels' average position plus their average
/// direction scaled to 1e10 over `(z*z + y*y) + x*x`.
fn forward_aim(cx: &Cx<'_>, h: ObjHandle, equip: SubObjId) -> Vector {
    let (p, d) = cx.world.launcher_barrel_average(h, equip).unwrap();
    let (x, y, z) = (e(d.x), e(d.y), e(d.z));
    let scale = 10_000_000_000.0 / ((z * z + y * y) + x * x).sqrt();
    let d = Vector::new(r(x * scale), r(y * scale), r(z * scale));
    Vector::new(r(e(p.x) + e(d.x)), r(e(d.y) + e(p.y)), r(e(d.z) + e(p.z)))
}

/// A point 1000 m along the barrels.
fn ahead(cx: &Cx<'_>, h: ObjHandle, equip: SubObjId) -> Vector {
    let (p, d) = cx.world.launcher_barrel_average(h, equip).unwrap();
    Vector::new(
        r(e(p.x) + e(d.x) * 1000.0),
        r(e(p.y) + e(d.y) * 1000.0),
        r(e(p.z) + e(d.z) * 1000.0),
    )
}

fn length(v: Vector) -> f64 {
    (e(v.x) * e(v.x) + e(v.y) * e(v.y) + e(v.z) * e(v.z)).sqrt()
}

fn close(a: Vector, b: Vector, tolerance: f64) -> bool {
    length(Vector::new(a.x - b.x, a.y - b.y, a.z - b.z)) <= tolerance
}

/// The launch velocity reaches the body at its first PSI
/// (`PhySys::AddToVelocity`): after a short physics step the body moves
/// along it, less what its damping (a mine's drag) took.
fn launched_with(cx: &mut Cx<'_>, h: ObjHandle, launch: Vector) {
    cx.world.step_physics(he01(), 0.02);
    let v = cx.world.velocity(h);
    let cos = (e(v.x) * e(launch.x) + e(v.y) * e(launch.y) + e(v.z) * e(launch.z))
        / (length(v) * length(launch));
    let ratio = length(v) / length(launch);
    assert!(
        cos > 0.9999 && ratio > 0.8 && ratio < 1.0001,
        "{v:?} launched at {launch:?}"
    );
}

#[test]
fn a_gun_fires_a_bullet_through_the_server() {
    let Some((mut world, mut logic)) = setup("gun fire") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let gun = item(&cx, h, types::GUN, Some(false));
    ready(&mut cx, h, gun);
    let aim = ahead(&cx, h, gun);
    // `ComputeLaunchInfo`'s muzzle: the first barrel's.
    let (muzzle, _) = cx.world.launcher_barrel_average(h, gun).unwrap();
    let speed = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.by_id(gun))
        .map(|e| match &e.kind {
            EquipKind::Gun(g) => g.launcher.muzzle_velocity,
            _ => 0.0,
        })
        .unwrap();
    let munition = cx.world.launcher_projectile(h, gun).unwrap().nickname;

    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[gun], Some(aim), 0),
        0
    );
    // `CELauncher::Fire` restarted the refire clock; the wrapper counted it.
    assert_eq!(refire_elapsed(&cx, h, gun), 0.0);
    assert_eq!(fire_count(&cx, h, gun), 1);
    let bullets = of_kind(&cx, ObjectKind::Beam);
    assert_eq!(bullets.len(), 1, "one barrel, one round");
    let b = cx.world.get(bullets[0]).unwrap();
    let KindData::Beam(beam) = &b.data else {
        panic!("not a bullet");
    };
    assert_eq!(beam.projectile.owner_id, id(&cx, h));
    assert_eq!(beam.munition, munition);
    assert_eq!(b.simple.id, 0, "a server bullet has no id");
    assert!(close(b.engine.position, muzzle, 1e-3));
    // At rest, the round leaves at the muzzle velocity (the dispersion
    // keeps the speed) towards the aim point.
    assert!((length(beam.velocity) - e(speed)).abs() < 1e-2);
    let toward = Vector::new(aim.x - muzzle.x, aim.y - muzzle.y, aim.z - muzzle.z);
    let cos = (e(beam.velocity.x) * e(toward.x)
        + e(beam.velocity.y) * e(toward.y)
        + e(beam.velocity.z) * e(toward.z))
        / (length(beam.velocity) * length(toward));
    assert!(cos > 0.99, "{cos}");
    assert!(cx.logic.objects.get(bullets[0]).is_some());

    // The refire delay refuses the next shot: nothing counted, no round.
    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[gun], Some(aim), 0),
        0
    );
    assert_eq!(fire_count(&cx, h, gun), 1);
    assert_eq!(of_kind(&cx, ObjectKind::Beam).len(), 1);

    // An item without a wrapper answers -2; a client's shot pays (counts)
    // and makes no round: the client makes its own.
    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[0x7ff0], Some(aim), 0),
        -2
    );
    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[gun], Some(aim), 1),
        0
    );
    assert_eq!(fire_count(&cx, h, gun), 2);
    assert_eq!(of_kind(&cx, ObjectKind::Beam).len(), 1);
}

#[test]
fn single_player_makes_no_bullet_on_the_server() {
    let config = WorldConfig {
        single_player: true,
        ..WorldConfig::default()
    };
    let Some((mut world, mut logic)) = setup_config("single-player gun fire", config) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let gun = item(&cx, h, types::GUN, Some(false));
    ready(&mut cx, h, gun);
    let aim = ahead(&cx, h, gun);
    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[gun], Some(aim), 0),
        0
    );
    assert_eq!(fire_count(&cx, h, gun), 1);
    assert_eq!(refire_elapsed(&cx, h, gun), 0.0);
    assert!(of_kind(&cx, ObjectKind::Beam).is_empty());
}

#[test]
fn a_missile_launcher_creates_a_guided_munition() {
    let Some((mut world, mut logic)) = setup("missile launch") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let target = fighter(&mut cx, Vector::new(BASE.x, BASE.y, BASE.z - 600.0), 0);
    let launcher = item(&cx, h, types::GUN, Some(true));
    ready(&mut cx, h, launcher);
    // A lock seeker with no lock time locks at once.
    assert!(
        cx.world
            .gun_set_target(h, launcher, Some(target), SUBOBJ_ID_ROOT)
    );
    let ammo = cx.world.launcher_projectile(h, launcher).unwrap();
    assert!(ammo.requires_ammo && ammo.seeker != 0);
    let rounds = stack(&cx, h, ammo.nickname);
    let aim = ahead(&cx, h, launcher);
    let (muzzle, _) = cx.world.launcher_barrel_average(h, launcher).unwrap();

    assert_eq!(
        equip_fire::fire_weapons(&mut cx, h, &[launcher], Some(aim), 0),
        0
    );
    assert_eq!(fire_count(&cx, h, launcher), 1);
    // `DrawAmmoFromCargo`: one round per barrel.
    assert_eq!(stack(&cx, h, ammo.nickname), rounds - 1);
    assert!(of_kind(&cx, ObjectKind::Beam).is_empty());
    let missiles = of_kind(&cx, ObjectKind::Guided);
    assert_eq!(missiles.len(), 1);
    let m = cx.world.get(missiles[0]).unwrap();
    let KindData::Guided(g) = &m.data else {
        panic!("not a missile");
    };
    assert_eq!(g.projectile.owner_id, id(&cx, h));
    assert_eq!(g.munition, ammo.nickname);
    assert_eq!((g.target, g.sub_target), (Some(target), SUBOBJ_ID_ROOT));
    assert_ne!(m.simple.id, 0, "a missile has an id");
    assert!(close(m.engine.position, muzzle, 1e-3));
    assert!(cx.logic.objects.get(missiles[0]).is_some());
}

#[test]
fn the_mine_dropper_launches_through_create_mine() {
    let Some((mut world, mut logic)) = setup("mine launch") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let dropper = item(&cx, h, types::MINE_DROPPER, None);
    ready(&mut cx, h, dropper);
    let mine_arch = cx.world.launcher_projectile(h, dropper).unwrap();
    let mines_held = stack(&cx, h, mine_arch.nickname);
    // Without an aim point the server's shot fires forward; a dropper's
    // launch record is `CELauncher::ComputeLaunchInfo_OneBarrel` (no draw).
    let aim = forward_aim(&cx, h, dropper);
    let expected = cx.world.launcher_launch_info(h, dropper, aim);
    assert_eq!(expected.len(), 1);

    assert_eq!(equip_fire::fire_weapons(&mut cx, h, &[dropper], None, 0), 0);
    assert_eq!(fire_count(&cx, h, dropper), 1);
    assert_eq!(refire_elapsed(&cx, h, dropper), 0.0);
    assert_eq!(
        stack(&cx, h, mine_arch.nickname),
        mines_held - u32::from(mine_arch.requires_ammo)
    );
    let mines = of_kind(&cx, ObjectKind::Mine);
    assert_eq!(mines.len(), 1);
    let m = cx.world.get(mines[0]).unwrap();
    let KindData::Mine(data) = &m.data else {
        panic!("not a mine");
    };
    assert_eq!(data.projectile.owner_id, id(&cx, h));
    assert_eq!(m.archetype, mine_arch.nickname);
    assert_eq!(m.engine.position, expected[0].pos);
    // `create_mine` gave it the server's mine state (`arm`).
    assert!(
        cx.logic
            .objects
            .get(mines[0])
            .and_then(|o| o.mine.as_ref())
            .is_some()
    );
    launched_with(&mut cx, mines[0], expected[0].velocity);
}

#[test]
fn the_counter_measure_dropper_launches_through_create_counter_measure() {
    let Some((mut world, mut logic)) = setup("counter-measure launch") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let dropper = item(&cx, h, types::CM_DROPPER, None);
    ready(&mut cx, h, dropper);
    let cm_arch = cx.world.launcher_projectile(h, dropper).unwrap();
    let held = stack(&cx, h, cm_arch.nickname);
    let aim = forward_aim(&cx, h, dropper);
    let expected = cx.world.launcher_launch_info(h, dropper, aim);

    assert_eq!(equip_fire::fire_weapons(&mut cx, h, &[dropper], None, 0), 0);
    assert_eq!(fire_count(&cx, h, dropper), 1);
    assert_eq!(
        stack(&cx, h, cm_arch.nickname),
        held - u32::from(cm_arch.requires_ammo)
    );
    let counters = of_kind(&cx, ObjectKind::CounterMeasure);
    assert_eq!(counters.len(), 1);
    let c = cx.world.get(counters[0]).unwrap();
    let KindData::CounterMeasure(data) = &c.data else {
        panic!("not a counter-measure");
    };
    assert_eq!(data.projectile.owner_id, id(&cx, h));
    assert_eq!(c.engine.position, expected[0].pos);
    launched_with(&mut cx, counters[0], expected[0].velocity);

    // `SrvCounterMeasureDropper::notify(0x19)` on an NPC's ship drops one
    // (`slot_34(&id, 1, null, -1)`); a player's ship ignores it.
    ready(&mut cx, h, dropper);
    equip_fire::equip_event(&mut cx, h, dropper, EquipEvent::IncomingMissile);
    assert_eq!(of_kind(&cx, ObjectKind::CounterMeasure).len(), 2);
    assert_eq!(fire_count(&cx, h, dropper), 2);
    ready(&mut cx, h, dropper);
    cx.world.get_mut(h).unwrap().simple.owner_player = 1;
    equip_fire::equip_event(&mut cx, h, dropper, EquipEvent::IncomingMissile);
    assert_eq!(of_kind(&cx, ObjectKind::CounterMeasure).len(), 2);
}

#[test]
fn a_starved_thruster_switches_an_npcs_thrusters_off() {
    let Some((mut world, mut logic)) = setup("thruster notice") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 0);
    let thruster = item(&cx, h, types::THRUSTER, None);
    let active = |cx: &Cx<'_>| {
        cx.world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(thruster))
            .is_some_and(|e| e.is_active())
    };
    assert_eq!(equip_fire::set_thrusters_active(&mut cx, h, true, 0), 0);
    assert!(active(&cx));
    equip_fire::equip_event(&mut cx, h, thruster, EquipEvent::ThrusterStarved);
    assert!(!active(&cx));
}

/// The id of the ship's archetype's bay hardpoints.
fn bay_names(cx: &Cx<'_>, h: ObjHandle) -> (Vec<u8>, Vec<u8>) {
    let arch = cx.world.get(h).unwrap().archetype;
    let s = cx.world.data().archetypes.ships.get(&arch).unwrap();
    (
        cx.world.strings().get(s.hp_bay_surface).to_vec(),
        cx.world.strings().get(s.hp_bay_external).to_vec(),
    )
}

#[test]
fn a_jettison_makes_loot_at_the_computed_launch_point() {
    let Some((mut world, mut logic)) = setup("jettison") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 5);
    let food = create_id(LUXURY_FOOD);
    assert_eq!(stack(&cx, h, food), 5);
    // Not a stack: -2.
    let gun = item(&cx, h, types::GUN, Some(false));
    assert_eq!(cargo::jettison(&mut cx, h, gun, 1, 0), -2);
    assert_eq!(cargo::jettison(&mut cx, h, CARGO_ID, 3, 0), 0);
    // Outside single player the units leave the hold at once; the bay
    // opens; the stack waits in the queue.
    assert_eq!(stack(&cx, h, food), 2);
    let queued = cargo::queued_jettison(&cx, h);
    assert_eq!(queued.len(), 1);
    assert_eq!((queued[0].arch_id, queued[0].count), (food, 3));

    let (surface, external) = bay_names(&cx, h);
    let (surface_pos, surface_orient) = cx.world.hardpoint_world(h, &surface).unwrap();
    let (external_pos, _) = cx.world.hardpoint_world(h, &external).unwrap();
    let velocity_constant = cx.world.data().constants.general.jettisoned_cargo_velocity;

    // Frames until the bay is open and the container leaves.
    let mut draws = None;
    for _ in 0..400 {
        let before = cx.world.rng().clone();
        world::step_system(&mut cx, he01(), 0.05);
        cx.world.update_global_time(0.05);
        if !of_kind(&cx, ObjectKind::Loot).is_empty() {
            draws = Some(before);
            break;
        }
    }
    let mut rng = draws.expect("the jettisoned container");
    assert_eq!(
        cx.world.get(h).and_then(|o| o.as_ship()).unwrap().bay_state,
        BayState::Open
    );
    let loot = of_kind(&cx, ObjectKind::Loot);
    assert_eq!(loot.len(), 1);

    // `compute_jettison_launch`: from the bay's surface hardpoint towards
    // its external one at `(rand() * 2^-15 * 0.5 + 0.75) *
    // JETTISONED_CARGO_VELOCITY`, plus the ship's velocity (at rest), the
    // first draw of the frame.
    let dir = Vector::new(
        r(e(external_pos.x) - e(surface_pos.x)),
        r(e(external_pos.y) - e(surface_pos.y)),
        r(e(external_pos.z) - e(surface_pos.z)),
    );
    let unit = 1.0f32 / 32768.0;
    let speed = (e(rng.rand() as f32 * unit) * 0.5 + 0.75) * e(velocity_constant);
    let scale = r(speed / length(dir));
    let launch = Vector::new(
        r(e(scale) * e(dir.x)),
        r(e(scale) * e(dir.y)),
        r(e(scale) * e(dir.z)),
    );

    let o = cx.world.get(loot[0]).unwrap();
    let KindData::Loot(l) = &o.data else {
        panic!("not loot");
    };
    assert_eq!(o.engine.position, surface_pos);
    assert_eq!(o.engine.orientation, surface_orient);
    // The contents, their units and hit points (health 1 of 100), the
    // container the commodity's loot appearance with its own hit points.
    assert_eq!((l.contents_arch, l.units), (food, 3));
    assert_eq!(l.contents_hit_pts, 100.0);
    assert_eq!(l.owner_id, id(&cx, h));
    assert_eq!(o.archetype, create_id(b"lootcrate_red"));
    assert_eq!(o.simple.hit_pts, 250.0);
    assert!(l.can_ai_tractor && !l.temporary);
    launched_with(&mut cx, loot[0], launch);
    // The stack left the queue; one bay close is owed after the clearance
    // time.
    let state = cx
        .logic
        .objects
        .get(h)
        .and_then(|o| o.ship.as_ref())
        .map(|s| s.jettison.clone())
        .unwrap();
    assert!(state.queue.is_empty());
    assert_eq!(state.pending_bay_closes, 1);
    assert_eq!(state.timer, game_sim::object::ship::JETTISON_CLEARANCE_TIME);
    for _ in 0..100 {
        world::step_system(&mut cx, he01(), 0.05);
        cx.world.update_global_time(0.05);
    }
    let state = cx
        .logic
        .objects
        .get(h)
        .and_then(|o| o.ship.as_ref())
        .map(|s| s.jettison.clone())
        .unwrap();
    assert_eq!((state.pending_bay_closes, state.timer), (0, 0.0));
    let bay = cx.world.get(h).and_then(|o| o.as_ship()).unwrap().bay_state;
    assert!(
        matches!(bay, BayState::Closing | BayState::Closed),
        "{bay:?}"
    );
}

fn stack_state(cx: &Cx<'_>, h: ObjHandle) -> (u32, f32, bool) {
    let e = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.by_id(CARGO_ID))
        .unwrap();
    match &e.kind {
        EquipKind::Cargo(c) => (c.count, c.hit_pts, e.destroyed),
        _ => panic!("not cargo"),
    }
}

fn set_stack(cx: &mut Cx<'_>, h: ObjHandle, count: u32, hit_pts: f32) {
    let e = cx
        .world
        .get_mut(h)
        .and_then(|o| o.equipped.as_deref_mut())
        .and_then(|eq| eq.equipment.by_id_mut(CARGO_ID))
        .unwrap();
    if let EquipKind::Cargo(c) = &mut e.kind {
        c.count = count;
        c.hit_pts = hit_pts;
    }
}

#[test]
fn a_cargo_stack_decays() {
    let Some((mut world, mut logic)) = setup("cargo decay") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 3);
    assert_eq!(stack_state(&cx, h), (3, 100.0, false));
    // `CargoDecayTimer` gathers 0.125 s a frame (one sub-step); 1 s is not
    // past 1 s, so the ninth frame (1.125 s) decays the stack by 1 point a
    // second times 1.125.
    for frame in 1..=9 {
        world::step_system(&mut cx, he01(), 0.125);
        cx.world.update_global_time(0.125);
        let expected = if frame < 9 { 100.0 } else { 98.875 };
        assert_eq!(stack_state(&cx, h).1, expected, "frame {frame}");
    }
    assert_eq!(cx.logic.objects.get(h).unwrap().cargo_decay_timer, 0.0);

    // A spent unit of a stack leaves it (`apply_equip_entry`'s rule): the
    // entry is 0 with no fate, the next unit is whole.
    set_stack(&mut cx, h, 3, 0.5);
    cargo::decay_cargo(&mut cx, h, 1.0);
    assert_eq!(stack_state(&cx, h), (2, 100.0, false));
    // A spent single unit is destroyed.
    set_stack(&mut cx, h, 1, 0.5);
    cargo::decay_cargo(&mut cx, h, 1.0);
    let (count, hit_pts, destroyed) = stack_state(&cx, h);
    assert_eq!((count, hit_pts), (1, 0.0));
    assert!(destroyed);

    // A valid owning player's open trade freezes the decay
    // (`Server/server_gameobject.cpp:4171-4175`).
    let h = fighter(&mut cx, BASE, 3);
    set_stack(&mut cx, h, 3, 50.0);
    cx.world.get_mut(h).unwrap().simple.owner_player = 1;
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.trade_offer = Some(game_logic::trade::TradeOffer::default());
    }
    cargo::decay_cargo(&mut cx, h, 1.0);
    assert_eq!(stack_state(&cx, h), (3, 50.0, false));
    cx.logic.players.get_mut(1).unwrap().trade_offer = None;
    cargo::decay_cargo(&mut cx, h, 1.0);
    assert_eq!(stack_state(&cx, h), (3, 49.0, false));
}

#[test]
fn the_stack_rule_is_the_servers() {
    let Some((mut world, mut logic)) = setup("the stack rule") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let h = fighter(&mut cx, BASE, 3);
    let mut list = DamageList::new(cargo::CAUSE_CARGO_DECAY, 0, 0);
    list.add_damage_entry(CARGO_ID, 0.0, Fate::None);
    // The bare world sets the hit points.
    cx.world
        .apply_damage_list_hooked(h, &list, &mut NoServerHooks);
    assert_eq!(stack_state(&cx, h), (3, 0.0, false));
    set_stack(&mut cx, h, 3, 40.0);
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    assert_eq!(stack_state(&cx, h), (2, 100.0, false));
    // A non-zero entry sets the top unit's hit points.
    let mut list = DamageList::new(cargo::CAUSE_CARGO_DECAY, 0, 0);
    list.add_damage_entry(CARGO_ID, 12.5, Fate::None);
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    assert_eq!(stack_state(&cx, h), (2, 12.5, false));
}
