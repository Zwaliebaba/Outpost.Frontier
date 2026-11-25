//! Content's NPC ships through the server (work package P3.6c, decision
//! record 0008 points 4 and 7) over the original data (`outdir/`, he01):
//! the oracle's start-up (Content's GameSupport, SpaceSupport and
//! BaseSupport, then `LoadGame` over `InitialWorld.ini`), he01 loaded, and
//! an NPC ship made by `FSystemInfo::CreateNpcShip` with a "nothing" guide.
//! Expected values follow the C++ (`Content/NpcShip.cpp`, `Guides.cpp`).
//!
//! - The ship is its manager's directive and content callback: one handle
//!   in both, the creation type its kind maps to.
//! - The guide starts at once: its 10000 s delay (priority 1) is the
//!   ship's one directive, running.
//! - The flee answers: a position (state 1), an object (2), a dock (3),
//!   and none from a guide without targets.
//! - The heartbeat runs the ship's life down; a destroyed ship leaves the
//!   record, the arena and the callbacks, and its guide is released.

use std::path::Path;
use std::sync::Arc;

use game_logic::content::callbacks::{self, CallbackOwner};
use game_logic::content::guides::GuideDesc;
use game_logic::content::npc_ships::{self, NpcShipParams};
use game_logic::content::ships::ShipKey;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::ai::content::FleeAnswer;
use game_sim::ai::state_graph::GraphType;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// he01 on the oracle's start-up, `None` without `outdir/`.
fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no NPC ship was made");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 4, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = controller::create(&mut cx, b"Content.dll", name, &CreateParams::None, priority);
        assert_ne!(id.0, 0);
    }
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    game_logic::selection::load_world_state(&mut cx, &mut reader);
    world::load(&mut cx, create_id(b"he01"));
    Some((world, logic))
}

fn fighter_params(cx: &Cx<'_>) -> NpcShipParams {
    NpcShipParams {
        controller: cx.logic.content.state.space_support.0,
        archetype: create_id(b"he_fighter"),
        loadout: create_id(b"li_p_he_fighter_loadout01"),
        position: Vector::new(15000.0, 3000.0, -25000.0),
        state_graph: cx
            .world
            .data()
            .state_graphs
            .find(b"FIGHTER", GraphType::Leader),
        guide: Some(GuideDesc {
            kind: 1,
            ..GuideDesc::default()
        }),
        ..NpcShipParams::default()
    }
}

#[test]
fn an_npc_ship_is_its_managers_callbacks_and_runs_its_guide() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let params = fighter_params(&cx);
    let next_handle = cx.logic.content.state.callbacks.peek_next();
    let (result, key) = npc_ships::create_in_system(&mut cx, he01, &params, 0);
    assert_eq!(result, 0);
    let key = key.expect("the ship is made");
    let ship = npc_ships::get(&cx, key).expect("in the arena").clone();
    assert_eq!(ship.callback, next_handle);
    assert_eq!(
        cx.logic.content.state.callbacks.owner(ship.callback),
        Some(CallbackOwner::NpcShip(key))
    );
    assert_eq!(cx.logic.content.state.systems[&he01].space_objs, vec![key]);
    let h = cx.world.find(ship.base.ship).expect("the space object");
    let m = cx.world.behavior_manager(h).expect("a manager");
    assert_eq!(m.state.content_handle, ship.callback);
    // Creation kind 2 answers creation type 0.
    assert_eq!(m.state.creation_type, 0);
    assert!(m.state.directive_callbacks.contains(&ship.callback));
    // The nothing guide's delay is the one directive, running.
    let q = npc_ships::queue(&cx, ShipKey::Npc(key)).unwrap();
    assert_eq!(q.list.len(), 1);
    assert_eq!(q.current, Some(0));
    assert_eq!(q.active, Some(q.list[0]));
    let guide = ship.guide.expect("a guide");
    assert_eq!(game_logic::content::guides::guide_type(&cx, guide), 1);

    // The flee answers.
    assert_eq!(callbacks::flee_answer(&cx, ship.callback), None);
    npc_ships::set_flee_position(&mut cx, key, Vector::new(1.0, 2.0, 3.0), 700.0);
    assert_eq!(
        callbacks::flee_answer(&cx, ship.callback),
        Some(FleeAnswer {
            kind: 3,
            position: Vector::new(1.0, 2.0, 3.0),
            radius: 700.0,
            ..FleeAnswer::default()
        })
    );
    npc_ships::set_flee_object(&mut cx, key, 77, 300.0, true);
    assert_eq!(
        callbacks::flee_answer(&cx, ship.callback).map(|a| (a.kind, a.param, a.radius)),
        Some((2, 77, 300.0))
    );
    npc_ships::clear_flee(&mut cx, key);

    // The ship's life is -1 (immortal): the heartbeat leaves it.
    let to = cx.logic.content.state.space_support;
    let _ = cx.dispatch(
        to,
        &mut game_logic::events::ControllerMessage::Heartbeat { elapsed: 1.0 },
    );
    assert!(npc_ships::get(&cx, key).is_some());

    // Destroyed: the record, the arena, the callbacks and the guide forget
    // it.
    let _ = game_logic::api::space_obj::destroy(&mut cx, ship.base.ship, 1);
    let _ = world::step_system(&mut cx, he01, 1.0 / 30.0);
    assert!(npc_ships::get(&cx, key).is_none());
    assert!(cx.logic.content.state.systems[&he01].space_objs.is_empty());
    assert_eq!(cx.logic.content.state.callbacks.owner(ship.callback), None);
    assert!(cx.logic.content.state.guides.get(guide).is_none());
}
