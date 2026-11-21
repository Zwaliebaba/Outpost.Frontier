//! The server's ship over the original data (`outdir/`): `pub::SpaceObj::
//! Create` (the ship in its system with its server entry, the info's
//! values, the result codes, the fuses its hit points light, the
//! transport loadout), the server equipment and the equipment state, the
//! launch invincibility, and `Ship::update`'s rules: the update flags, the
//! zone damage, the weapons-offline repair, a repair droid, and the death
//! sequence with the dying-ship notice. Expected values follow the C++
//! (`Server/server_gameobject.cpp`, `server_pub.cpp`).

use std::cell::RefCell;
use std::path::Path;
use std::sync::Arc;

use game_logic::api::PubError;
use game_logic::api::space_obj::{self, CargoDesc, ShipInfo};
use game_logic::events::ControllerMessage;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::ship::{self, ServerEquipKind};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{EquipKind, Fate, SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT, types};
use game_sim::object::damage::{CAUSE_GUN, DamageList};
use game_sim::object::world::{Assets, ServerHooks, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World, update_flags};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

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

fn setup_with(what: &str, config: WorldConfig) -> Option<(World, Logic)> {
    let data = data(what)?;
    let world = World::new(data, config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::Msg0x25 {
                ship,
                death_sequence,
                system,
            } => format!("Msg0x25 {ship} {death_sequence} {system}"),
            ControllerMessage::LaunchComplete { id, ship } => format!("LaunchComplete {id} {ship}"),
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

fn setup(what: &str) -> Option<(World, Logic)> {
    setup_with(what, WorldConfig::default())
}

/// A Content controller: its id owns the ships, and the bus delivers to the
/// hook.
fn content(cx: &mut Cx<'_>) -> u32 {
    neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0
}

fn info(controller: u32, system: u32, arch: &[u8], loadout: &[u8], at: Vector) -> ShipInfo {
    let yaw = 0.3f32;
    let orientation = Matrix::from_rows([
        [yaw.cos(), 0.0, yaw.sin()],
        [0.0, 1.0, 0.0],
        [-yaw.sin(), 0.0, yaw.cos()],
    ]);
    ShipInfo::new(
        controller,
        system,
        create_id(arch),
        create_id(loadout),
        at,
        orientation,
    )
}

fn create(cx: &mut Cx<'_>, info: &ShipInfo) -> ObjHandle {
    let id = space_obj::create(cx, info).expect("the ship is made");
    cx.world.find(id).expect("the world has it")
}

fn apply(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, list, &mut hooks);
}

fn lit(cx: &Cx<'_>, h: ObjHandle) -> Vec<u32> {
    cx.logic
        .objects
        .get(h)
        .map(|o| o.fuses.iter().map(|f| f.fuse.nickname).collect())
        .unwrap_or_default()
}

/// One server frame of the system and the global time after it.
fn frame(cx: &mut Cx<'_>, system: u32, dt: f32) {
    world::step_system(cx, system, dt);
    cx.world.update_global_time(dt);
}

#[test]
fn create_makes_the_ship_in_its_system_with_its_server_entry() {
    let Some((mut world, mut logic)) = setup("ship creation") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let _ = heard();

    let mut fighter = info(
        controller,
        he01,
        b"he_fighter",
        b"li_p_he_fighter_loadout01",
        Vector::new(1000.0, 200.0, -3000.0),
    );
    fighter.velocity = Vector::new(0.0, 0.0, 10.0);
    fighter.vibe = 7;
    fighter.voice = create_id(b"pilot_f_leg_f01a");
    fighter.docking_base = create_id(b"he01_01_Base");
    fighter.group_name_id = 0x1234;
    fighter.hit_pts = 900;
    fighter.mark_player = 2;
    fighter.costume.part_ids = [1, 2, 3, 4];
    let id = space_obj::create(&mut cx, &fighter).expect("the ship is made");
    let h = cx.world.find(id).expect("found by its id");

    // In the system's ship list, with the info's values.
    assert!(cx.world.objects_in(he01).contains(&h));
    let o = cx.world.get(h).unwrap();
    assert_eq!(o.simple.owner_player, 0);
    assert_eq!(o.simple.hit_pts, 900.0, "the integer hit points");
    assert_eq!(o.engine.position, fighter.position);
    assert_eq!(
        o.engine.orientation,
        game_sim::scenario::spawn_orientation(&fighter.orientation),
        "the orientation through the quaternion"
    );
    let eq = o.equipped.as_deref().unwrap();
    assert_eq!(eq.vibe, 7);
    assert_eq!(eq.voice, fighter.voice);
    assert_eq!(eq.docking_base, fighter.docking_base);
    assert_eq!(eq.space_costume.part_ids, [1, 2, 3, 4]);
    assert_eq!(o.as_ship().unwrap().group_name_id, 0x1234);

    // Recorded with its controller.
    assert!(cx.logic.space_obj.created_ships.contains(&id));
    assert_eq!(
        cx.logic.space_obj.ship_controllers.get(&id),
        Some(&controller)
    );

    // The server entry: the mark, the `Ship` state with the loadout's id
    // maker, and one wrapper per gun, dropper, thruster and tractor, in
    // traversal order.
    let entry = cx.logic.objects.get(h).expect("the server entry");
    assert!(entry.is_marked_by(2));
    let state = entry.ship.as_ref().expect("the ship state");
    assert_eq!(state.death_sequence_obj_id, 0);
    let loadout = cx
        .world
        .data()
        .loadouts
        .get(fighter.loadout)
        .unwrap()
        .id_maker
        .clone();
    assert_eq!(state.equip_ids, loadout, "no cargo: the loadout's maker");
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    let expected: Vec<(u16, u32)> = eq
        .equipment
        .traverse(types::ALL, false)
        .into_iter()
        .filter_map(|s| eq.equipment.get(s))
        .filter(|e| {
            matches!(
                e.type_bits,
                types::GUN
                    | types::CM_DROPPER
                    | types::MINE_DROPPER
                    | types::THRUSTER
                    | types::REPAIR_DROID
                    | types::TRACTOR
            )
        })
        .map(|e| (e.id, e.type_bits))
        .collect();
    let wrappers: Vec<(u16, u32)> = entry
        .server_equip
        .iter()
        .map(|s| {
            let t = match s.kind {
                ServerEquipKind::Gun(_) => types::GUN,
                ServerEquipKind::CounterMeasureDropper(_) => types::CM_DROPPER,
                ServerEquipKind::MineDropper(_) => types::MINE_DROPPER,
                ServerEquipKind::Thruster => types::THRUSTER,
                ServerEquipKind::RepairDroid { .. } => types::REPAIR_DROID,
                ServerEquipKind::Tractor => types::TRACTOR,
            };
            (s.equip, t)
        })
        .collect();
    assert_eq!(wrappers, expected);
    let count = |t: u32| wrappers.iter().filter(|w| w.1 == t).count();
    assert_eq!(count(types::GUN), 4, "three guns and the missile launcher");
    assert_eq!(count(types::CM_DROPPER), 1);
    assert_eq!(count(types::THRUSTER), 1);
    assert_eq!(count(types::TRACTOR), 1);

    // The result codes.
    let mut bad = fighter.clone();
    bad.controller = controller + 1000;
    assert_eq!(space_obj::create(&mut cx, &bad), Err(PubError(-4)));
    bad = fighter.clone();
    bad.system = 0;
    assert_eq!(space_obj::create(&mut cx, &bad), Err(PubError(-1)));
    bad.system = create_id(b"no_such_system");
    assert_eq!(space_obj::create(&mut cx, &bad), Err(PubError(-1)));
    bad = fighter.clone();
    bad.archetype = create_id(b"no_such_ship");
    assert_eq!(space_obj::create(&mut cx, &bad), Err(PubError(-1)));
    bad = fighter.clone();
    bad.system = create_id(b"he02");
    assert_eq!(
        space_obj::create(&mut cx, &bad),
        Err(PubError(-1)),
        "a system that is not loaded"
    );
    assert_eq!(cx.logic.space_obj.created_ships.len(), 1);
}

#[test]
fn create_checks_the_new_ships_fuses() {
    let Some((mut world, mut logic)) = setup("fuses of a new ship") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let _ = heard();

    // At full health nothing lights.
    let full = create(
        &mut cx,
        &info(
            controller,
            he01,
            b"ge_transport",
            b"ge_transport",
            Vector::new(0.0, 0.0, -20000.0),
        ),
    );
    assert!(lit(&cx, full).is_empty());

    // 300 hit points are at or below the 750 and 375 thresholds of the
    // archetype's intermediate fuses, not the death fuse's 1.
    let mut damaged = info(
        controller,
        he01,
        b"ge_transport",
        b"ge_transport",
        Vector::new(0.0, 0.0, -22000.0),
    );
    damaged.hit_pts = 300;
    let h = create(&mut cx, &damaged);
    assert_eq!(
        lit(&cx, h),
        [
            create_id(b"intermed_damage_transport01"),
            create_id(b"intermed_damage_transport02")
        ]
    );
    assert!(heard().is_empty(), "no death fuse");

    // 1 hit point lights the death fuse too, which Content hears.
    damaged.hit_pts = 1;
    damaged.position.z = -24000.0;
    let h = create(&mut cx, &damaged);
    assert_eq!(lit(&cx, h)[0], create_id(b"transport_death_fuse"));
    assert_eq!(heard(), ["DeathFuseLit"]);
    assert!(cx.logic.objects.get(h).unwrap().death_fuse_lit);
}

#[test]
fn the_transport_loadout_fills_the_cargo_pods() {
    let Some((mut world, mut logic)) = setup("transport loadout") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let cargo = [
        CargoDesc {
            good: create_id(b"commodity_gold"),
            count: 7,
        },
        CargoDesc {
            good: create_id(b"commodity_boron"),
            count: 4,
        },
        CargoDesc {
            good: create_id(b"commodity_cobalt"),
            count: 3,
        },
    ];
    let transport = create_id(b"ge_transport");
    let maker = cx
        .world
        .data()
        .loadouts
        .get(create_id(b"ge_transport"))
        .unwrap()
        .id_maker
        .clone();
    let mut ids = maker.clone();
    let mut out = Vec::new();
    space_obj::build_transport_loadout(&mut cx, transport, &mut ids, &cargo, &mut out);

    // Two hardpoints, two lines: per hardpoint a pod and its share; the
    // last share takes the rest; the third line is not loaded.
    let pod = |good: &[u8]| match &cx.world.data().archetypes.equipment[&create_id(good)].kind {
        game_sim::data::archetype::equipment::EquipmentKind::Commodity {
            pod_appearance, ..
        } => pod_appearance.unwrap(),
        _ => unreachable!(),
    };
    let hp1: &[u8] = b"HpCargo01";
    let hp2: &[u8] = b"HpCargo02";
    let gold = create_id(b"commodity_gold");
    let boron = create_id(b"commodity_boron");
    let lines = |cx: &Cx<'_>,
                 out: &[game_sim::data::equip_desc::EquipDesc]|
     -> Vec<(u32, Vec<u8>, u8, i32)> {
        out.iter()
            .map(|d| {
                let hp = cx.world.strings().get(d.hardpoint).to_vec();
                (d.arch_id, hp, d.equipped, d.count)
            })
            .collect()
    };
    let got = lines(&cx, &out);
    let (hp1, hp2) = (hp1.to_vec(), hp2.to_vec());
    assert_eq!(
        got,
        [
            (pod(b"commodity_gold"), hp1.clone(), 1, 1),
            (gold, hp1.clone(), 0, 3),
            (pod(b"commodity_gold"), hp2.clone(), 1, 1),
            (gold, hp2.clone(), 0, 4),
            (pod(b"commodity_boron"), hp1.clone(), 1, 1),
            (boron, hp1, 0, 2),
            (pod(b"commodity_boron"), hp2.clone(), 1, 1),
            (boron, hp2, 0, 2),
        ]
    );
    let mut next = maker.clone();
    for d in &out {
        assert_eq!(d.id, next.create(), "the maker's ids in order");
        assert_eq!(d.health, 1.0);
    }
    assert_eq!(ids, next);

    // A good without a pod appearance is reported and ends the loadout.
    let mut ids = maker.clone();
    let mut out = Vec::new();
    space_obj::build_transport_loadout(
        &mut cx,
        transport,
        &mut ids,
        &[CargoDesc {
            good: create_id(b"lair_power_cell"),
            count: 2,
        }],
        &mut out,
    );
    assert!(out.is_empty());
    let report = cx.logic.reports.0.last().expect("a report");
    assert!(
        format!("{report:?}").contains("has no pod_appearance.  Transport loadout aborted."),
        "{report:?}"
    );

    // Without cargo-pod hardpoints every line goes into the hold.
    let mut ids = maker.clone();
    let mut out = Vec::new();
    space_obj::build_transport_loadout(
        &mut cx,
        create_id(b"he_fighter"),
        &mut ids,
        &cargo,
        &mut out,
    );
    assert!(
        out.iter()
            .all(|d| d.hardpoint == cx.world.strings().cargo_bay()),
        "in the hold"
    );
    let bay = b"BAY".to_vec();
    assert_eq!(
        lines(&cx, &out),
        [
            (gold, bay.clone(), 0, 7),
            (boron, bay.clone(), 0, 4),
            (create_id(b"commodity_cobalt"), bay, 0, 3)
        ]
    );

    // `Create` puts the loadout on the ship and gives it the maker.
    let mut with_cargo = info(
        controller,
        he01,
        b"ge_transport",
        b"ge_transport",
        Vector::new(0.0, 0.0, -20000.0),
    );
    with_cargo.cargo = cargo.to_vec();
    let h = create(&mut cx, &with_cargo);
    let mut expected = maker.clone();
    let ids: Vec<u16> = (0..8).map(|_| expected.create()).collect();
    let state = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
    assert_eq!(state.equip_ids, expected);
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    for (i, id) in ids.iter().enumerate() {
        let item = eq.equipment.by_id(*id).expect("the item");
        if i % 2 == 1 {
            let EquipKind::Cargo(c) = &item.kind else {
                panic!("cargo");
            };
            assert_eq!(c.count, [3, 4, 2, 2][i / 2]);
        } else {
            assert_eq!(item.type_bits, types::CARGO_POD);
        }
    }

    // Its next equipment id continues the maker; a player's ship may not
    // allocate one.
    assert_eq!(ship::allocate_equip_id(&mut cx, h), expected.create());
    cx.world.get_mut(h).unwrap().simple.owner_player = 1;
    assert_eq!(ship::allocate_equip_id(&mut cx, h), SUBOBJ_ID_NONE);
    assert!(
        format!("{:?}", cx.logic.reports.0.last().unwrap())
            .contains("Player tried to allocate id!")
    );
}

#[test]
fn the_equipment_state_and_the_launch_invincibility() {
    let Some((mut world, mut logic)) = setup("equipment state") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let h = create(
        &mut cx,
        &info(
            controller,
            he01,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            Vector::new(0.0, 0.0, -20000.0),
        ),
    );
    let id = cx.world.get(h).unwrap().simple.id;

    // `set_equip_state` takes the maker and the data word, and rebuilds
    // the server equipment.
    let mut maker = neuron_core::id::sub_object::EquipmentIds::default();
    for _ in 0..40 {
        maker.create();
    }
    if let Some(o) = cx.logic.objects.entry_mut(h) {
        o.server_equip.clear();
    }
    ship::set_equip_state(
        &mut cx,
        h,
        &ship::EquipState {
            equip_ids: maker.clone(),
            data: 0xbeef,
        },
    );
    let entry = cx.logic.objects.get(h).unwrap();
    assert_eq!(entry.server_equip.len(), 7);
    let state = entry.ship.as_ref().unwrap();
    assert_eq!((state.equip_ids.clone(), state.data), (maker, 0xbeef));

    // The launch invincibility: once, against everyone, no loss allowed;
    // the launch's end lifts it and tells Content.
    ship::apply_launch_invincibility(&mut cx, h);
    let entry = cx.logic.objects.get(h).unwrap();
    assert!(entry.invincible && entry.invincible_flag);
    assert_eq!(entry.invincible_timer, 0.0);
    assert_eq!(entry.invincibilities.len(), 1);
    ship::apply_launch_invincibility(&mut cx, h);
    assert_eq!(cx.logic.objects.get(h).unwrap().invincibilities.len(), 1);
    let _ = heard();
    assert_eq!(ship::launch_complete(&mut cx, h, 77), 0);
    assert_eq!(heard(), [format!("LaunchComplete 77 {id}")]);
    let entry = cx.logic.objects.get(h).unwrap();
    assert!(!entry.invincible && !entry.invincible_flag);
    assert_eq!(entry.invincible_timer, 1.0);
    assert_eq!(ship::launch_complete(&mut cx, h, 77), 0);
    assert!(heard().is_empty(), "only once");
}

#[test]
fn an_npc_updates_without_its_scanner_and_a_player_dumb() {
    let Some((mut world, mut logic)) = setup("update flags") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let h = create(
        &mut cx,
        &info(
            controller,
            he01,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            Vector::new(0.0, 0.0, -20000.0),
        ),
    );
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    assert_eq!(
        hooks.object_update_flags(cx.world, h),
        update_flags::NO_SCANNER
    );
    cx.world.get_mut(h).unwrap().simple.owner_player = 1;
    assert_eq!(
        hooks.object_update_flags(cx.world, h),
        update_flags::UPDATE_DUMB
    );
    let solar = cx.world.load_system_solars(he01)[0];
    assert_eq!(hooks.object_update_flags(cx.world, solar), 0);
}

/// The radiation cloud of Ve04 (`Zone_ve04_LD14_radioactive`, damage 80).
const RADIATION: Vector = Vector::new(-38713.0, 0.0, -67918.0);

#[test]
fn a_ship_in_a_damaging_zone_takes_its_share_every_frame_after_the_first_second() {
    let Some((mut world, mut logic)) = setup("zone damage") else {
        return;
    };
    let ve04 = create_id(b"ve04");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(ve04);
    let h = create(
        &mut cx,
        &info(
            controller,
            ve04,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            RADIATION,
        ),
    );
    let total = cx.world.get(h).unwrap().total_max_hit_pts();
    let max = cx.world.get(h).unwrap().simple.max_hit_pts;
    // The two fins are the hull's health proxies: their share goes to the
    // hull as well (`damage_group`), after the hull's own.
    let mut proxies: Vec<(u16, f64)> = cx
        .world
        .get(h)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .arch_groups
        .iter()
        .filter(|g| g.root_health_proxy)
        .map(|g| (g.id, g.max_hit_points()))
        .collect();
    proxies.sort_by_key(|p| p.0);
    assert_eq!(proxies.len(), 2);
    // Frames of 0.125 s: one sub-step each.
    let dt = 0.125f32;
    // `AccumulatedDamage += rate * immersion * dt`, the time by dt; with
    // more than 1 s and more than 1 % of the total, the fraction of the
    // hull's maximum goes, then the fins' fractions.
    let (mut acc, mut time, mut hull) = (0.0f32, 0.0f32, max);
    for step in 0..14 {
        frame(&mut cx, ve04, dt);
        let o = cx.world.get(h).unwrap();
        assert_eq!(
            o.simple.zone,
            Some(create_id(b"Zone_ve04_LD14_radioactive")),
            "step {step}"
        );
        let immersion = o.simple.scanner_interference;
        assert!(immersion > 0.0);
        acc = (80.0 * f64::from(immersion) * f64::from(dt) + f64::from(acc)) as f32;
        time = (f64::from(dt) + f64::from(time)) as f32;
        if time > 1.0 {
            let ratio = (f64::from(acc) / f64::from(total)) as f32;
            if ratio > 0.01 {
                let damage = (f64::from(max) * f64::from(ratio)) as f32;
                hull = (f64::from(hull) - f64::from(damage)) as f32;
                for (_, fin) in &proxies {
                    let damage = (fin * f64::from(ratio)) as f32;
                    hull = (f64::from(hull) - f64::from(damage)) as f32;
                }
                acc = 0.0;
            }
        }
        assert_eq!(o.simple.hit_pts, hull, "step {step}");
        let state = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
        assert_eq!(state.accumulated_damage, acc, "step {step}");
        assert_eq!(state.accumulated_damage_time, time, "step {step}");
    }
    assert!(hull < max, "the zone damaged the hull");

    // An invincible ship is spared.
    let spared = create(
        &mut cx,
        &info(
            controller,
            ve04,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            Vector::new(RADIATION.x + 100.0, 0.0, RADIATION.z),
        ),
    );
    if let Some(o) = cx.logic.objects.entry_mut(spared) {
        o.invincible = true;
    }
    for _ in 0..8 {
        frame(&mut cx, ve04, dt);
    }
    assert_eq!(cx.world.get(spared).unwrap().simple.hit_pts, max);
    let state = cx.logic.objects.get(spared).unwrap().ship.clone().unwrap();
    assert_eq!(state.accumulated_damage_time, 0.0);
}

#[test]
fn weapons_offline_repairs_the_hull_and_the_disabled_launchers_after_a_minute() {
    let Some((mut world, mut logic)) = setup("weapons-offline repair") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let h = create(
        &mut cx,
        &info(
            controller,
            he01,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            Vector::new(0.0, 0.0, -20000.0),
        ),
    );
    let gun = {
        let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
        eq.equipment.first(types::GUN).unwrap().id
    };
    cx.world
        .get_mut(h)
        .unwrap()
        .equipped
        .as_deref_mut()
        .unwrap()
        .weapons_offline = true;
    for _ in 0..4 {
        frame(&mut cx, he01, 1.0);
    }

    // A hit that empties the hull and the gun: the object is kept (weapons
    // offline), and both damage times are stamped, as both had health.
    let hit_at = cx.world.time();
    let mut list = DamageList::new(CAUSE_GUN, 0, 0);
    list.add_damage_entry(SUBOBJ_ID_ROOT, 0.0, Fate::None);
    list.add_damage_entry(gun, 0.0, Fate::None);
    list.destroyed = true;
    apply(&mut cx, h, &list);
    let o = cx.world.get(h).unwrap();
    assert!(!o.dead);
    assert_eq!(o.simple.hit_pts, 0.0);
    let item = o.equipped.as_deref().unwrap().equipment.by_id(gun).unwrap();
    assert_eq!(item.hit_points(), 0.0);
    assert!(item.is_disabled(true));
    let entry = cx.logic.objects.get(h).unwrap();
    assert_eq!(entry.last_damage_time, hit_at);
    let wrapper = entry.server_equip.iter().find(|s| s.equip == gun).unwrap();
    assert_eq!(wrapper.launcher().unwrap().last_damage_time, hit_at);

    // Nothing comes back while 60 s have not passed since.
    let max = cx.world.get(h).unwrap().simple.max_hit_pts;
    loop {
        let before = cx.world.time();
        frame(&mut cx, he01, 1.0);
        let o = cx.world.get(h).unwrap();
        if before - hit_at > 60.0 {
            assert_eq!(o.simple.hit_pts, max, "repaired at {before}");
            let item = o.equipped.as_deref().unwrap().equipment.by_id(gun).unwrap();
            assert_eq!(item.hit_points(), item.max_hit_points());
            break;
        }
        assert_eq!(o.simple.hit_pts, 0.0, "not yet at {before}");
    }
}

#[test]
fn a_repair_droid_repairs_the_most_damaged_item_every_two_seconds() {
    let Some((mut world, mut logic)) = setup("repair droid") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(he01);
    let h = create(
        &mut cx,
        &info(
            controller,
            he01,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            Vector::new(0.0, 0.0, -20000.0),
        ),
    );
    // No archetype of the data has a repair droid; one is made from the
    // scanner's archetype with a repair rate of 10, active, and wrapped.
    let droid_id = ship::allocate_equip_id(&mut cx, h);
    let arch = cx.world.data().archetypes.equipment[&create_id(b"ge_s_scanner_02")].clone();
    let mut droid = game_sim::equip::Equip::new(
        droid_id,
        &arch,
        types::REPAIR_DROID,
        EquipKind::RepairDroid(game_sim::equip::misc::RepairDroid { repair_rate: 10.0 }),
        false,
    );
    droid.activate_base(true);
    let (gun, gun_max) = {
        let eq = cx
            .world
            .get_mut(h)
            .unwrap()
            .equipped
            .as_deref_mut()
            .unwrap();
        eq.equipment.add(droid);
        let gun = eq.equipment.first(types::GUN).unwrap();
        (gun.id, gun.max_hit_points())
    };
    ship::rebuild_server_equip(&mut cx, h);
    let mut list = DamageList::new(CAUSE_GUN, 0, 0);
    let current = (f64::from(gun_max) * 0.5) as f32;
    list.add_damage_entry(gun, current, Fate::None);
    apply(&mut cx, h, &list);

    // The first update picks the gun; with frames of 0.125 s (one sub-step
    // each) the timer passes 2 s at the seventeenth, and the gun gains
    // 10 × 2.125 up to its maximum (its hit points over its relative
    // health, stored to floats).
    let gun_hp = |cx: &Cx<'_>| {
        cx.world
            .get(h)
            .unwrap()
            .equipped
            .as_deref()
            .unwrap()
            .equipment
            .by_id(gun)
            .unwrap()
            .hit_points()
    };
    for step in 0..16 {
        frame(&mut cx, he01, 0.125);
        assert_eq!(gun_hp(&cx), current, "step {step}");
    }
    let wrapper = |cx: &Cx<'_>| {
        cx.logic
            .objects
            .get(h)
            .unwrap()
            .server_equip
            .iter()
            .find(|s| s.equip == droid_id)
            .unwrap()
            .kind
    };
    assert_eq!(
        wrapper(&cx),
        ServerEquipKind::RepairDroid {
            timer: 2.0,
            target: gun
        }
    );
    frame(&mut cx, he01, 0.125);
    let relative = (f64::from(current) / f64::from(gun_max)) as f32;
    let max = (f64::from(current) / f64::from(relative)) as f32;
    let repaired = f64::from((10.0f64 * 2.125) as f32) + f64::from(current);
    let expected = if repaired < f64::from(max) {
        repaired as f32
    } else {
        max
    };
    assert_eq!(gun_hp(&cx), expected);
    assert_eq!(
        wrapper(&cx),
        ServerEquipKind::RepairDroid {
            timer: 0.0,
            target: gun
        }
    );
}

#[test]
fn a_death_sequence_skips_the_update_and_ends_on_the_dying_list() {
    let Some((mut world, mut logic)) = setup("death sequence") else {
        return;
    };
    let ve04 = create_id(b"ve04");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    cx.world.load_system(ve04);
    let h = create(
        &mut cx,
        &info(
            controller,
            ve04,
            b"he_fighter",
            b"li_p_he_fighter_loadout01",
            RADIATION,
        ),
    );
    let id = cx.world.get(h).unwrap().simple.id;
    // The docking part's `complete_dock` sets the sequence: the gate's id,
    // collisions off and a timer (here 1 s).
    if let Some(s) = cx.logic.objects.entry_mut(h).and_then(|o| o.ship.as_mut()) {
        s.death_sequence_obj_id = 0xabc;
        s.death_sequence_timer = 1.0;
    }
    cx.world.set_collisions(h, false);
    let collisions = |cx: &Cx<'_>| {
        let (system, body) = cx.world.body(h).unwrap();
        cx.world.physics(system).unwrap().collision_state(body)
    };
    for step in 0..3 {
        frame(&mut cx, ve04, 0.25);
        let o = cx.world.get(h).unwrap();
        assert_eq!(o.simple.zone, None, "no own update, step {step}");
        let s = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
        assert_eq!(s.death_sequence_obj_id, 0xabc);
        assert_eq!(s.accumulated_damage_time, 0.0, "no zone damage");
        assert!(cx.logic.server_world.dying_ships.is_empty());
        assert!(!collisions(&cx));
    }
    // The timer runs out at the fourth frame: collisions back on, the
    // sequence over, the ship on the dying list.
    frame(&mut cx, ve04, 0.25);
    let s = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
    assert_eq!((s.death_sequence_obj_id, s.death_sequence_timer), (0, 0.0));
    assert_eq!(cx.logic.server_world.dying_ships, [h]);
    assert!(collisions(&cx));
    // After the frame the list is announced and emptied.
    let _ = heard();
    world::physics_update(&mut cx, 0.25);
    assert_eq!(heard(), [format!("Msg0x25 {id} 0 {ve04}")]);
    assert!(cx.logic.server_world.dying_ships.is_empty());
    // The next frame updates the ship again.
    frame(&mut cx, ve04, 0.25);
    assert!(cx.world.get(h).unwrap().simple.zone.is_some());

    // A player's ship on a multiplayer server waits in its sequence.
    cx.world
        .get_mut(h)
        .unwrap()
        .as_ship_mut()
        .unwrap()
        .is_player = true;
    if let Some(s) = cx.logic.objects.entry_mut(h).and_then(|o| o.ship.as_mut()) {
        s.death_sequence_obj_id = 0xabc;
        s.death_sequence_timer = 0.1;
    }
    for _ in 0..4 {
        frame(&mut cx, ve04, 0.25);
    }
    let s = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
    assert_eq!(
        (s.death_sequence_obj_id, s.death_sequence_timer),
        (0xabc, 0.1)
    );
    assert!(cx.logic.server_world.dying_ships.is_empty());
}

#[test]
fn single_player_finds_the_players_ship_and_leaves_its_update_to_the_client() {
    let config = WorldConfig {
        single_player: true,
        step_mode: StepMode::SinglePlayer,
        ..WorldConfig::default()
    };
    let Some((mut world, mut logic)) = setup_with("single-player ship", config) else {
        return;
    };
    let ve04 = create_id(b"ve04");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    cx.world.load_system(ve04);
    // The client side made the player's ship.
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        RADIATION,
        Matrix::IDENTITY,
    );
    spawn.owner_player = 1;
    let client = cx.world.spawn_ship(ve04, &spawn).unwrap();
    let id = cx.world.get(client).unwrap().simple.id;
    let count = cx.world.objects_in(ve04).len();
    let request = ship::ShipCreateRequest {
        id,
        archetype: spawn.archetype,
        loadout: spawn.loadout,
        position: RADIATION,
        orientation: neuron_core::math::matrix_to_quaternion(&Matrix::IDENTITY),
        velocity: Vector::ZERO,
        angular_velocity: Vector::ZERO,
        owner_player: 1,
        hit_pts: 500,
        equipment: Vec::new(),
        vibe: -1,
        costume: Default::default(),
        voice: 0,
        docking_base: 0,
        group_name_id: 0,
        rank: 0,
        using_tradelane: false,
        mark_player: 0,
        equip_ids: Default::default(),
        equip_state_data: 0,
    };
    let h = ship::create_ship(&mut cx, ve04, &request).expect("found");
    assert_eq!(h, client);
    assert_eq!(cx.world.objects_in(ve04).len(), count, "nothing new");
    let max = cx.world.get(h).unwrap().simple.max_hit_pts;
    assert_eq!(cx.world.get(h).unwrap().simple.hit_pts, max, "not re-made");
    assert!(cx.logic.objects.get(h).unwrap().ship.is_some());
    assert_eq!(cx.logic.objects.get(h).unwrap().server_equip.len(), 7);

    // The server does not update it (the zone stays the client's), but the
    // zone damage still runs after the client's update would.
    for _ in 0..2 {
        frame(&mut cx, ve04, 0.25);
    }
    assert_eq!(cx.world.get(h).unwrap().simple.zone, None);
    let s = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
    assert_eq!(s.accumulated_damage_time, 0.5);
    assert_eq!(s.accumulated_damage, 0.0, "no zone without the client");
    // A death sequence ends on the dying list once the ship jumped out.
    if let Some(s) = cx.logic.objects.entry_mut(h).and_then(|o| o.ship.as_mut()) {
        s.death_sequence_obj_id = 0xabc;
    }
    frame(&mut cx, ve04, 0.25);
    assert!(cx.logic.server_world.dying_ships.is_empty());
    cx.world
        .get_mut(h)
        .unwrap()
        .as_ship_mut()
        .unwrap()
        .jumped_out = true;
    frame(&mut cx, ve04, 0.25);
    assert_eq!(cx.logic.server_world.dying_ships, [h]);
    let s = cx.logic.objects.get(h).unwrap().ship.clone().unwrap();
    assert_eq!(s.death_sequence_obj_id, 0);
}
