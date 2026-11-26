//! Work package P3.11b (decision O52, amended 2026-11-26) over the original
//! data (`outdir/`), on a multiplayer server: an item destroyed with a
//! loot fate drops its container (`EqObj::destroy_equip` and
//! `build_equip_loot`, `Server/server_gameobject.cpp:3365-3488`,
//! `4058-4153`), which `EquipDropLoot` lets Content take away, and the
//! destroyed item leaves its ship at once (`CEqObj::destroy_sub_objs` and
//! `CEquipManager::CleanUp`, `:1433-1441`).
//!
//! Expected values follow the C++ logic.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, Cx, Logic};
use game_logic::{objects, world};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{Fate, SubObjId, types};
use game_sim::object::damage::{self, DamageList};
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::object::{KindData, ObjHandle, ObjectKind, World};
use game_sim::protocol::ToClient;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;

struct Sink;

impl ClientSink for Sink {
    fn send(&mut self, _client: u32, _message: ToClient) {}
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(name: &str) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {name}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(Sink), 3, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::EquipDropLoot(e) => format!(
                "EquipDropLoot object {:x} inflictor {:x} cause {} count {} health {}",
                e.object, e.inflictor, e.cause, e.desc.count, e.desc.health
            ),
            ControllerMessage::LootSpawned { good, count, .. } => {
                format!("LootSpawned good {good:x} count {count}")
            }
            _ => return,
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// An NPC fighter of the police loadout (three guns) in he01, owned by
/// Content's GameSupport.
fn fighter(cx: &mut Cx<'_>) -> (ObjHandle, u32) {
    let controller = neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &game_logic::server::CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0;
    world::load(cx, he01());
    let info = game_logic::api::space_obj::ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(15000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    let id = game_logic::api::space_obj::create(cx, &info).expect("the ship");
    (cx.world.find(id).unwrap(), id)
}

/// The ship's first gun: its id and archetype.
fn first_gun(cx: &Cx<'_>, h: ObjHandle) -> (SubObjId, u32) {
    let eq = cx.world.get(h).and_then(|o| o.equipped.as_deref()).unwrap();
    let gun = eq.equipment.first(types::GUN).expect("a gun");
    (gun.id, gun.arch_id)
}

/// `destroy_equip(item, fate)` in a list of `cause` from `inflictor`,
/// applied through the server's hooks.
fn destroy(cx: &mut Cx<'_>, h: ObjHandle, item: SubObjId, fate: Fate, inflictor: u32) {
    let mut list = DamageList::new(0x15, inflictor, 0);
    let state = objects::damage_state(&cx.logic.objects, h, 0);
    cx.world.with_damage_target_state(h, state, |t, _, _| {
        damage::destroy_equip(t, item, fate, &mut list);
    });
    let mut hooks = world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
}

fn containers(cx: &Cx<'_>) -> Vec<ObjHandle> {
    cx.world
        .objects_in(he01())
        .into_iter()
        .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Loot))
        .collect()
}

#[test]
fn a_gun_destroyed_with_the_loot_fate_drops_its_container() {
    let Some((mut world, mut logic)) = setup("equip-loot") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (ship, ship_id) = fighter(&mut cx);
    let (gun, arch) = first_gun(&cx, ship);
    let max = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&arch)
        .unwrap()
        .root
        .hit_pts;
    let _ = heard();
    // `build_equip_loot` draws five times: the jitter, three spins and
    // the spin's size.
    let mut expected = cx.world.rng().clone();
    for _ in 0..5 {
        expected.rand();
    }
    destroy(&mut cx, ship, gun, Fate::Loot, 0x1234);
    assert_eq!(*cx.world.rng(), expected);
    // Fate 4 asks the controllers with the item's description (no
    // controller takes it away here); the container follows.
    let good = cx.world.data().goods.arch_to_good(arch);
    assert_eq!(
        heard(),
        [
            format!("EquipDropLoot object {ship_id:x} inflictor 1234 cause 21 count 1 health 1"),
            format!("LootSpawned good {good:x} count 1"),
        ]
    );
    let made = containers(&cx);
    assert_eq!(made.len(), 1);
    let o = cx.world.get(made[0]).unwrap();
    assert_eq!(o.archetype, arch);
    let KindData::Loot(l) = &o.data else {
        panic!("a container");
    };
    assert_eq!((l.contents_arch, l.units), (arch, 1));
    assert_eq!(l.contents_hit_pts, max);
    // Outside single player the destroyed gun leaves the ship at once.
    let eq = cx
        .world
        .get(ship)
        .and_then(|o| o.equipped.as_deref())
        .unwrap();
    assert!(eq.equipment.by_id(gun).is_none());
}

#[test]
fn a_silent_loot_fate_asks_nobody_and_a_separation_drops_nothing() {
    let Some((mut world, mut logic)) = setup("equip-loot-silent") else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (ship, _) = fighter(&mut cx);
    let (gun, _) = first_gun(&cx, ship);
    let _ = heard();
    destroy(&mut cx, ship, gun, Fate::LootSilent, 0);
    let lines = heard();
    assert_eq!(lines.len(), 1, "{lines:?}");
    assert!(lines[0].starts_with("LootSpawned"));
    assert_eq!(containers(&cx).len(), 1);
    // A separated item vanishes from a server's ship without a container.
    let eq = cx
        .world
        .get(ship)
        .and_then(|o| o.equipped.as_deref())
        .unwrap();
    let other = eq
        .equipment
        .traverse(types::GUN, false)
        .into_iter()
        .find_map(|s| eq.equipment.get(s))
        .map(|e| e.id)
        .expect("another gun");
    let before = cx.world.rng().clone();
    destroy(&mut cx, ship, other, Fate::Separated, 0);
    assert_eq!(*cx.world.rng(), before);
    assert!(heard().is_empty());
    assert_eq!(containers(&cx).len(), 1);
    let eq = cx
        .world
        .get(ship)
        .and_then(|o| o.equipped.as_deref())
        .unwrap();
    assert!(eq.equipment.by_id(other).is_none());
}
