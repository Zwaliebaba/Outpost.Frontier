//! The server's star systems over the original data (`outdir/`, record
//! 0007): systems load when their first player arrives, every session
//! hears a multiplayer player's system, a player's removal destroys its
//! ships, the dock queue lands ships after a sub-step's deletes, the tick
//! steps each occupied system once, and a ship moves between systems; in
//! single player a landing unloads the system.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

fn data() -> Option<Arc<GameData>> {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    Some(Arc::new(data))
}

fn new_world(data: Arc<GameData>, config: WorldConfig) -> World {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    World::new(data, config).with_assets(Assets::new(outdir.join("data")))
}

fn logic(sink: &Shared, players: &[(u32, u32)], multiplayer: bool) -> Logic {
    let mut logic = Logic::new(Box::new(sink.clone()), 4, multiplayer);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::SystemLoaded { system } => format!("SystemLoaded {system}"),
            ControllerMessage::SystemUnloaded { system } => format!("SystemUnloaded {system}"),
            ControllerMessage::Msg0x27 { base, land, .. } => format!("Msg0x27 {base} {land}"),
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    for &(session, system) in players {
        let slot = logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = system;
    }
    logic
}

fn ship(cx: &mut Cx<'_>, system: u32, owner: u32, x: f32) -> ObjHandle {
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(x, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    spawn.owner_player = owner;
    cx.world
        .spawn_ship(system, &spawn)
        .expect("he_fighter spawns")
}

#[test]
fn multiplayer_systems_load_step_and_transfer() {
    let Some(data) = data() else {
        println!("outdir/ is missing: no systems");
        return;
    };
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let mut world = new_world(data, WorldConfig::default());
    let sink = Shared::default();
    let mut logic = logic(&sink, &[(1, he01), (2, he01), (3, he02)], true);
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
    world::initialize(&mut cx);
    let systems = cx.world.data().universe.systems.len();
    assert_eq!(cx.logic.server_world.systems.len(), systems);
    assert!(!cx.logic.server_world.system(he01).unwrap().loaded);

    // The first player loads he01; every session hears its system.
    world::add_player(&mut cx, he01, 1);
    let sys = cx.logic.server_world.system(he01).unwrap();
    assert!(sys.loaded && sys.has_player(1));
    let solars = cx.world.objects_in(he01).len();
    assert!(solars > 0, "the system file's solars");
    assert!(!cx.world.asteroid_fields(he01).is_empty(), "he01's fields");
    assert_eq!(heard(), [format!("SystemLoaded {he01}")]);
    let to: Vec<u32> = sink.0.borrow_mut().drain(..).map(|(c, _)| c).collect();
    assert_eq!(to, [1, 2, 3]);
    world::add_player(&mut cx, he01, 2);
    assert!(heard().is_empty(), "loaded once");
    world::add_player(&mut cx, he01, 0x4000_0000);
    let sys = cx.logic.server_world.system(he01).unwrap();
    assert_eq!((sys.count_players(0), sys.count_players(1)), (2, 1));

    // Removing a player destroys its ships in the system.
    let own = ship(&mut cx, he01, 1, 15000.0);
    let npc = ship(&mut cx, he01, 0, 15500.0);
    world::remove_player(&mut cx, he01, 1);
    assert!(!cx.logic.server_world.system(he01).unwrap().has_player(1));
    assert!(cx.world.get(own).unwrap().dead);
    assert!(!cx.world.get(npc).unwrap().dead);

    // The dock queue: an NPC lands and goes; player 2's ship lands and
    // player 2 leaves the system.
    let landing = ship(&mut cx, he01, 2, 16000.0);
    let base = create_id(b"he01_01_base");
    world::queue_dock(&mut cx, he01, Some(npc), None, base, true);
    world::queue_dock(&mut cx, he01, Some(landing), None, base, true);
    sink.0.borrow_mut().clear();
    assert_eq!(world::step_system(&mut cx, he01, 0.1), 1);
    let lines = heard();
    assert_eq!(lines.len(), 2, "{lines:?}");
    assert!(lines.iter().all(|l| *l == format!("Msg0x27 {base} true")));
    assert!(
        cx.world.get(npc).is_none_or(|o| o.dead),
        "the NPC is destroyed"
    );
    let sys = cx.logic.server_world.system(he01).unwrap();
    assert!(sys.dock_queue.is_empty() && sys.observers.is_empty());
    assert!(sys.loaded, "a multiplayer server keeps the system");

    // A ship from he01 to he02 through transit.
    world::add_player(&mut cx, he02, 3);
    let traveller = ship(&mut cx, he01, 0, 17000.0);
    let parms = world::remove_ship(&mut cx, traveller).expect("leaves");
    assert!(parms.mass > 0.0);
    assert!(cx.world.in_transit(traveller));
    let id = cx.world.get(traveller).unwrap().simple.id;
    assert_eq!(cx.world.find(id), Some(traveller));
    let at = Vector::new(0.0, 0.0, 0.0);
    assert!(world::add_ship(
        &mut cx,
        he02,
        traveller,
        at,
        Matrix::IDENTITY
    ));
    assert!(cx.world.objects_in(he02).contains(&traveller));

    // The tick steps he01 and he02 once each; time is the frame's.
    let before = cx.world.time();
    world::step_systems(&mut cx, 0.1);
    assert_eq!(cx.world.time(), before, "the frame loop owns the time");
    // Shutdown unloads.
    world::shutdown(&mut cx, he02);
    assert!(!cx.world.system_loaded(he02));
    assert!(cx.world.get(traveller).is_none());
    assert!(heard().contains(&format!("SystemUnloaded {he02}")));
}

#[test]
fn single_player_unloads_on_landing() {
    let Some(data) = data() else {
        println!("outdir/ is missing: no systems");
        return;
    };
    let he02 = create_id(b"he02");
    let mut world = new_world(
        data,
        WorldConfig {
            single_player: true,
            step_mode: StepMode::SinglePlayer,
            server_physics: false,
            ..WorldConfig::default()
        },
    );
    let sink = Shared::default();
    let mut logic = logic(&sink, &[(1, he02)], false);
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
    // The client side built he02's solars in the shared world.
    cx.world.load_system(he02);
    let solars = cx.world.load_system_solars(he02).len();
    world::add_player(&mut cx, he02, 1);
    assert_eq!(cx.world.objects_in(he02).len(), solars, "found, not made");
    assert!(sink.0.borrow().is_empty(), "no system id in single player");
    let player = ship(&mut cx, he02, 1, 0.0);
    let _ = heard();
    world::queue_dock(
        &mut cx,
        he02,
        Some(player),
        None,
        create_id(b"he02_01_base"),
        true,
    );
    world::step_system(&mut cx, he02, 0.1);
    let lines = heard();
    assert!(lines[0].starts_with("Msg0x27"), "{lines:?}");
    assert_eq!(lines[1], format!("SystemUnloaded {he02}"));
    let sys = cx.logic.server_world.system(he02).unwrap();
    assert!(!sys.loaded && sys.observers.is_empty());
    assert!(!cx.world.system_loaded(he02));

    // Leaving for another system also unloads, after `SystemExit`.
    world::add_player(&mut cx, he02, 1);
    let _ = heard();
    world::player_switched_out(&mut cx, he02, 1, create_id(b"he03"));
    let lines = heard();
    assert!(lines[0].starts_with("SystemExit"), "{lines:?}");
    assert_eq!(lines[1], format!("SystemUnloaded {he02}"));
}
