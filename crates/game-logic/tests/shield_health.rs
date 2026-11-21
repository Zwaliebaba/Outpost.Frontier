//! A shield's health through the item over the original data (`outdir/`):
//! `CEShield::GetMaxHitPoints` and `GetHitPoints` answer the shield's
//! generators' capacity and its current hit points, so `CEquip::
//! GetRelativeHealth` is the shield's own, and the damage state a server
//! sends (`EqObj::get_damage_list`, `Server/server_gameobject.cpp`) lists
//! a shield below the ceiling with those hit points.

use std::path::Path;
use std::sync::Arc;

use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::replication;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world::Hooks;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{EquipKind, Fate, types};
use game_sim::object::damage::{CAUSE_GUN, DamageList};
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::object::{ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no shield health");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.deliver_hook = Some(|_, _, _, _| {});
    Some((world, logic))
}

fn shield(cx: &Cx<'_>, h: ObjHandle) -> game_sim::equip::Equip {
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    let slots = eq.equipment.traverse(types::SHIELD, false);
    assert_eq!(slots.len(), 1, "the fighter has one shield");
    eq.equipment.get(slots[0]).cloned().unwrap()
}

fn damage_state(cx: &Cx<'_>, h: ObjHandle, ceiling: f32) -> Vec<(u16, f32, Fate)> {
    let mut list = DamageList::new(replication::CAUSE_DAMAGE_STATE, 0, 0);
    replication::get_damage_list(cx.world, h, &mut list, ceiling);
    list.entries
        .iter()
        .map(|e| (e.sub_obj_id, e.hit_pts, e.fate))
        .collect()
}

#[test]
fn a_shields_health_and_damage_state_are_its_generators() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let he01 = create_id(b"he01");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = neuron_server::controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0;
    cx.world.load_system(he01);
    let info = ShipInfo::new(
        controller,
        he01,
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(1000.0, 200.0, -3000.0),
        Matrix::IDENTITY,
    );
    let id = space_obj::create(&mut cx, &info).expect("the ship is made");
    let h = cx.world.find(id).expect("the world has it");

    // A full shield: its maximum is its generators' capacity, not its
    // archetype's hit points.
    let item = shield(&cx, h);
    let EquipKind::Shield(s) = &item.kind else {
        panic!("a shield");
    };
    let capacity = s.max_hit_pts;
    assert!(capacity > 0.0, "the shield has generators");
    assert_ne!(
        capacity, item.arch.hit_pts,
        "the capacity differs from the archetype's hit points"
    );
    assert_eq!(s.hit_pts, capacity);
    assert_eq!(item.max_hit_points(), capacity);
    assert_eq!(item.hit_points(), capacity);
    assert_eq!(item.relative_health(), 1.0);

    // At full health the shield is below a ceiling of 2 (as for a
    // retried damage state) with its current hit points, and not below 1.
    let shield_entry =
        |entries: &[(u16, f32, Fate)]| entries.iter().find(|e| e.0 == item.id).map(|e| (e.1, e.2));
    assert_eq!(
        shield_entry(&damage_state(&cx, h, 2.0)),
        Some((capacity, Fate::None))
    );
    assert_eq!(shield_entry(&damage_state(&cx, h, 1.0)), None);

    // A shield at 60 % of its capacity (above any offline threshold).
    let hit = (f64::from(capacity) * 0.6) as f32;
    let mut list = DamageList::new(CAUSE_GUN, 0, 0);
    list.add_damage_entry(item.id, hit, Fate::None);
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    let item = shield(&cx, h);
    assert_eq!(item.hit_points(), hit);
    assert_eq!(item.max_hit_points(), capacity);
    assert_eq!(item.relative_health(), f64::from(hit) / f64::from(capacity));
    assert_eq!(
        shield_entry(&damage_state(&cx, h, 1.0)),
        Some((hit, Fate::None))
    );

    // `pub::SpaceObj::GetShieldHealth` agrees.
    let health = space_obj::shield_health(&cx, id).unwrap().unwrap();
    assert_eq!((health.current, health.max), (hit, capacity));
}
