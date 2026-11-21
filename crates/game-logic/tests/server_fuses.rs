//! The server's fuses over the original data (`outdir/`): the fuse
//! database, a transport's death fuse lit by the damage that would destroy
//! it (it stays while the fuse burns, for Content's longer death
//! conversation, and `destroy_root` destroys it), a fighter's `death_comm`
//! from `EqObj::destroy`, a fighter's intermediate damage fuses lit at
//! their hit-point thresholds and unlit by healing, and `ignite_fuse`
//! chaining with its unlighting.

use std::cell::{Cell, RefCell};
use std::path::Path;
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::fuses::{self, FuseActionKind};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{Fate, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static LIFETIME: Cell<f32> = const { Cell::new(0.0) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

fn setup(what: &str) -> Option<(World, Logic)> {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::DeathFuseLit(e) => {
                e.lifetime = LIFETIME.with(Cell::get);
                "DeathFuseLit".to_owned()
            }
            ControllerMessage::ObjectDestroyed(e) => format!("ObjectDestroyed {}", e.id),
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

/// A Content controller, so that the bus delivers to the hook.
fn content(cx: &mut Cx<'_>) {
    neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    );
}

fn ship(cx: &mut Cx<'_>, system: u32, arch: &[u8], loadout: &[u8], z: f32) -> ObjHandle {
    let spawn = ShipSpawn::new(
        create_id(arch),
        create_id(loadout),
        Vector::new(15000.0, 3000.0, z),
        Matrix::IDENTITY,
    );
    cx.world
        .spawn_ship(system, &spawn)
        .expect("the ship spawns")
}

fn hit(cx: &mut Cx<'_>, h: ObjHandle, inflictor: u32, hull: f32) {
    let mut list = DamageList::new(game_sim::object::damage::CAUSE_GUN, inflictor, 0);
    list.add_damage_entry(SUBOBJ_ID_ROOT, hull, Fate::None);
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
}

fn lit(cx: &Cx<'_>, h: ObjHandle) -> Vec<u32> {
    cx.logic
        .objects
        .get(h)
        .map(|o| o.fuses.iter().map(|f| f.fuse.nickname).collect())
        .unwrap_or_default()
}

fn burning(cx: &Cx<'_>, h: ObjHandle, fuse: &[u8]) -> bool {
    let id = create_id(fuse);
    cx.logic.objects.get(h).is_some_and(|o| {
        o.fuses
            .iter()
            .any(|f| f.fuse.nickname == id && f.fuse.is_burning())
    })
}

fn gone(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world.get(h).is_none_or(|o| o.dead)
}

#[test]
fn the_fuse_database_loads_every_fuse() {
    let Some((mut world, mut logic)) = setup("fuse database") else {
        return;
    };
    let host = Host::new(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"));
    // The names of the listed files' `[fuse]` sections, read as text.
    let mut names = std::collections::BTreeSet::new();
    for path in &world.data().fuse_files.0 {
        let text = game_sim::data::DataSource::read(&host, path).expect("a listed fuse file");
        let text = String::from_utf8_lossy(&text).into_owned();
        let mut in_fuse = false;
        for line in text.lines() {
            let line = line.split(';').next().unwrap_or("").trim();
            if line.starts_with('[') {
                in_fuse = line.eq_ignore_ascii_case("[fuse]");
            } else if in_fuse
                && let Some(value) = line.strip_prefix("name")
                && let Some(value) = value.trim_start().strip_prefix('=')
            {
                names.insert(create_id(value.trim().as_bytes()));
            }
        }
    }
    let cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut cx = cx;
    let db = fuses::fuse_db(&mut cx);
    assert!(db.reports.is_empty(), "{:?}", db.reports);
    let ids: std::collections::BTreeSet<u32> = db.fuses.keys().copied().collect();
    assert_eq!(ids, names, "one prototype per fuse name");
    assert!(ids.len() > 150, "{} fuses", ids.len());

    let transport = db.get(create_id(b"transport_death_fuse")).expect("it");
    assert!(transport.fuse.death_fuse && transport.has_destroy_root);
    assert_eq!(transport.fuse.lifetime, 2.5);
    assert!(matches!(
        transport.fuse.actions.last().map(|(_, a)| &a.kind),
        Some(FuseActionKind::DestroyRoot)
    ));
    let comm = db.get(fuses::death_comm_fuse()).expect("death_comm");
    assert!(comm.has_destroy_root && comm.fuse.needs_randomize);
    assert!(matches!(
        comm.fuse.actions.first().map(|(_, a)| &a.kind),
        Some(FuseActionKind::Tumble { .. })
    ));
    let body = db.get(create_id(b"b_battleship_body_fuse")).expect("it");
    let ignites = body
        .fuse
        .actions
        .iter()
        .filter(|(_, a)| matches!(a.kind, FuseActionKind::IgniteFuse { .. }))
        .count();
    assert!(ignites >= 2, "{ignites} ignite_fuse actions");
    assert!(cx.logic.reports.0.is_empty(), "{:?}", cx.logic.reports);
}

#[test]
fn a_transport_burns_its_death_fuse_before_it_goes() {
    let Some((mut world, mut logic)) = setup("death fuse") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    cx.world.load_system(he01);
    let transport = ship(&mut cx, he01, b"ge_transport", b"ge_transport", -25000.0);
    let id = cx.world.get(transport).unwrap().simple.id;
    LIFETIME.with(|l| l.set(5.0));
    let _ = heard();

    // A hit that empties the hull lights the archetype's death fuse at
    // its 1-hit-point threshold: `DeathFuseLit` once, Content's longer
    // lifetime, and the object is kept.
    hit(&mut cx, transport, 0, 0.0);
    assert_eq!(heard(), ["DeathFuseLit"]);
    assert!(!gone(&cx, transport), "it stays while the fuse burns");
    assert!(burning(&cx, transport, b"transport_death_fuse"));
    assert!(burning(&cx, transport, b"intermed_damage_transport01"));
    assert!(burning(&cx, transport, b"intermed_damage_transport02"));
    let entry = cx.logic.objects.get(transport).unwrap();
    assert_eq!(entry.fuses[0].fuse.lifetime, 5.0, "Content's answer");
    assert!(entry.death_fuse_lit);
    assert!(fuses::is_death_fuse_burning(&cx.logic.objects, transport));
    // A second empty hit (cause below 0x10) does not destroy it either.
    hit(&mut cx, transport, 0, 0.0);
    assert!(!gone(&cx, transport));
    assert!(heard().is_empty());

    // The burn: the fuse's actions run as the frames reach them, and
    // `destroy_root` at its end destroys the transport (no second
    // `DeathFuseLit`).
    let mut elapsed = 0.0f32;
    while !gone(&cx, transport) && elapsed < 10.0 {
        world::step_system(&mut cx, he01, 0.25);
        elapsed += 0.25;
        if elapsed <= 4.5 {
            assert!(!gone(&cx, transport), "gone after {elapsed} s");
        }
    }
    assert!(gone(&cx, transport), "destroy_root ends it");
    assert!(
        (4.75..=5.5).contains(&elapsed),
        "destroyed after {elapsed} s"
    );
    let heard = heard();
    assert!(
        heard.contains(&format!("ObjectDestroyed {id}")),
        "{heard:?}"
    );
    assert!(!heard.iter().any(|l| l == "DeathFuseLit"), "{heard:?}");
}

#[test]
fn an_npc_destroyed_by_damage_lights_death_comm() {
    let Some((mut world, mut logic)) = setup("death_comm") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    cx.world.load_system(he01);
    let fighter = ship(
        &mut cx,
        he01,
        b"he_fighter",
        b"li_p_he_fighter_loadout01",
        -25000.0,
    );
    let id = cx.world.get(fighter).unwrap().simple.id;

    // Without a death conversation the NPC goes at once.
    LIFETIME.with(|l| l.set(0.0));
    let other = ship(
        &mut cx,
        he01,
        b"he_fighter",
        b"li_p_he_fighter_loadout01",
        -26000.0,
    );
    let _ = heard();
    hit(&mut cx, other, 0, 0.0);
    assert!(gone(&cx, other));
    assert!(!lit(&cx, other).contains(&fuses::death_comm_fuse()));

    // With one, `EqObj::destroy` lights `death_comm` for its length and
    // keeps the ship, which tumbles and is destroyed at the fuse's end.
    LIFETIME.with(|l| l.set(3.0));
    let _ = heard();
    hit(&mut cx, fighter, 0, 0.0);
    let first = heard();
    assert_eq!(first[0], "DeathFuseLit", "{first:?}");
    assert!(!first.iter().any(|l| l.starts_with("ObjectDestroyed")));
    assert!(!gone(&cx, fighter));
    assert!(burning(&cx, fighter, b"death_comm"));
    let comm = cx
        .logic
        .objects
        .get(fighter)
        .unwrap()
        .fuses
        .iter()
        .find(|f| f.fuse.nickname == fuses::death_comm_fuse())
        .unwrap();
    assert_eq!(comm.fuse.lifetime, 3.0);
    assert!(!comm.fuse.needs_randomize, "the ranges were drawn");
    world::step_system(&mut cx, he01, 0.25);
    let ship = cx.world.get(fighter).unwrap().as_ship().unwrap();
    assert!(
        (ship.flight.angular_drag_factor - 0.3).abs() < 1e-6,
        "the tumble"
    );
    assert_eq!(ship.flight.throttle, 1.0);
    let mut elapsed = 0.25f32;
    while !gone(&cx, fighter) && elapsed < 10.0 {
        world::step_system(&mut cx, he01, 0.25);
        elapsed += 0.25;
    }
    assert!(
        (2.75..=3.5).contains(&elapsed),
        "destroyed after {elapsed} s"
    );
    assert!(heard().contains(&format!("ObjectDestroyed {id}")));
}

#[test]
fn damage_fuses_light_at_their_thresholds_and_healing_unlights_them() {
    let Some((mut world, mut logic)) = setup("damage fuses") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    cx.world.load_system(he01);
    let fighter = ship(
        &mut cx,
        he01,
        b"he_fighter",
        b"li_p_he_fighter_loadout01",
        -25000.0,
    );
    let two = create_id(b"intermed_damage_smallship02");
    let three = create_id(b"intermed_damage_smallship03");

    // 1300 to 350: through the 400 threshold only.
    hit(&mut cx, fighter, 0, 350.0);
    assert_eq!(lit(&cx, fighter), [two]);
    assert!(burning(&cx, fighter, b"intermed_damage_smallship02"));
    // 350 to 150: through 200.
    hit(&mut cx, fighter, 0, 150.0);
    assert_eq!(lit(&cx, fighter), [two, three]);
    // A hit that stays above both thresholds lights nothing again.
    hit(&mut cx, fighter, 0, 120.0);
    assert_eq!(lit(&cx, fighter), [two, three]);
    // Healing to 450 unlights both; the fuses stay in the list.
    hit(&mut cx, fighter, 0, 450.0);
    assert_eq!(cx.world.get(fighter).unwrap().simple.hit_pts, 450.0);
    assert_eq!(lit(&cx, fighter), [two, three]);
    assert!(!burning(&cx, fighter, b"intermed_damage_smallship02"));
    assert!(!burning(&cx, fighter, b"intermed_damage_smallship03"));
    // Damage lights the same copy again.
    hit(&mut cx, fighter, 0, 300.0);
    assert_eq!(lit(&cx, fighter), [two, three]);
    assert!(burning(&cx, fighter, b"intermed_damage_smallship02"));
    assert!(!burning(&cx, fighter, b"intermed_damage_smallship03"));
    assert!(!fuses::is_death_fuse_burning(&cx.logic.objects, fighter));
}

#[test]
fn ignite_fuse_chains_in_the_same_frame_and_unlights_with_its_fuse() {
    let Some((mut world, mut logic)) = setup("fuse chains") else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    cx.world.load_system(he01);
    let fighter = ship(
        &mut cx,
        he01,
        b"he_fighter",
        b"li_p_he_fighter_loadout01",
        -25000.0,
    );
    LIFETIME.with(|l| l.set(0.0));
    let body = create_id(b"b_battleship_body_fuse");
    let tower = create_id(b"b_battleship_cntrl_twr_fuse");
    let head = create_id(b"b_battleship_head_fuse");

    // An unknown fuse is reported and not lit.
    let missing = create_id(b"no_such_fuse");
    assert!(!fuses::light_fuse(
        &mut cx,
        fighter,
        0,
        missing,
        SUBOBJ_ID_ROOT,
        0.0,
        -1.0
    ));
    let report = cx.logic.reports.0.last().expect("a report");
    assert!(report.text.contains("not found in FuseDB"), "{report:?}");

    assert!(fuses::light_fuse(
        &mut cx,
        fighter,
        0,
        body,
        SUBOBJ_ID_ROOT,
        0.0,
        -1.0
    ));
    // The body (12 s) passes its `ignite_fuse` for the tower at 0.1; the
    // tower is appended and burns in the same frame (0.6 s of its 1 s).
    let mut frames = 0;
    while !lit(&cx, fighter).contains(&tower) {
        fuses::burn_fuses(&mut cx, fighter, 0.6);
        frames += 1;
        assert!(frames < 10, "the tower never lit");
    }
    assert_eq!(lit(&cx, fighter), [body, tower], "the head waits for 0.2");
    let entry = cx.logic.objects.get(fighter).unwrap();
    let tower_burn = entry.fuses[1].fuse.burns[0];
    assert_eq!(tower_burn.upper, 0.6, "it burned in the frame it was lit");
    assert_eq!(tower_burn.lower, 0.0);
    let body_upper = entry.fuses[0].fuse.burns[0].upper;
    assert!((0.1..0.2).contains(&body_upper), "{body_upper}");

    // Unlighting the body untriggers its `ignite_fuse`, which unlights
    // the tower.
    assert!(fuses::unlight_fuse(
        &mut cx,
        fighter,
        body,
        SUBOBJ_ID_ROOT,
        0.0
    ));
    let entry = cx.logic.objects.get(fighter).unwrap();
    assert!(entry.fuses[0].fuse.burns.is_empty());
    assert!(entry.fuses[1].fuse.burns.is_empty(), "the tower is out");
    let ignite = entry.fuses[0]
        .fuse
        .actions
        .iter()
        .find(|(_, a)| matches!(a.kind, FuseActionKind::IgniteFuse { fuse, .. } if fuse == tower))
        .unwrap();
    assert!(!ignite.1.base.triggered, "an ignite_fuse can fire again");
    // A fuse the object never lit is not found.
    assert!(!fuses::unlight_fuse(
        &mut cx,
        fighter,
        head,
        SUBOBJ_ID_ROOT,
        0.0
    ));

    // Lit again, the chain fires again.
    assert!(fuses::light_fuse(
        &mut cx,
        fighter,
        0,
        body,
        SUBOBJ_ID_ROOT,
        0.0,
        -1.0
    ));
    for _ in 0..frames {
        fuses::burn_fuses(&mut cx, fighter, 0.6);
    }
    assert!(burning(&cx, fighter, b"b_battleship_cntrl_twr_fuse"));
}
