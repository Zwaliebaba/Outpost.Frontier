//! SpaceSupport's dock queues and formations through the server (work
//! package P3.6a, decision record 0008 point 7) over the original data
//! (`outdir/`, the rebranded he01): the oracle's start-up (Content's
//! GameSupport, SpaceSupport and BaseSupport, then `LoadGame` over
//! `InitialWorld.ini`), he01 loaded, and ships asking station he01_11 for
//! a dock (`DockRequest` to the bus, `Content/DockQueues.cpp`'s
//! `HandleDockRequest`). Expected values follow the C++.
//!
//! - A ship Content does not know (an NPC ship the server made itself)
//!   is refused with 5 at port -1, as `dock_at_station`'s fighter is in
//!   the oracle.
//! - A player's ship is granted the nearest free berth (4), enters the
//!   docking list and is queued at the station; its `DockComplete`
//!   finishes the entry and frees the terminal, which the handler's tail
//!   hands to `pub::SpaceObj::Activate`.
//! - With every berth held, a player's ship waits (3, port -1) in the
//!   waiting list, and the next finished dock grants it the freed berth.
//! - A formation request between players who are not grouped is refused
//!   (-8), and one from a ship Content does not know is -3.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::space_obj::{self, ShipInfo};
use game_logic::content::players::{PlayerShip, PlayerState};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

impl Shared {
    /// The requests' answers sent so far: (object, type, response, port).
    fn returned(&self) -> Vec<(u32, u32, i32, i32)> {
        self.0
            .borrow_mut()
            .drain(..)
            .filter_map(|(_, m)| match m {
                ToClient::Server(ServerMessage::RequestReturned(r)) => Some((
                    r.object_id,
                    u32::from(r.request_type),
                    i32::from(r.response as i8),
                    i32::from(r.field_26 as i8),
                )),
                _ => None,
            })
            .collect()
    }
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// he01 on the oracle's start-up, `None` without `outdir/`.
fn setup(what: &str, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, true);
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
    world::load(&mut cx, he01());
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

fn station(cx: &Cx<'_>) -> (ObjHandle, u32) {
    let h = cx.world.find(create_id(b"he01_11")).expect("he01_11");
    (h, id(cx, h))
}

/// A fighter made through `pub::SpaceObj::Create` beside the station.
fn fighter(cx: &mut Cx<'_>, x: f32) -> (ObjHandle, u32) {
    // Owned by SpaceSupport, a live controller.
    let owner = cx.logic.content.state.space_support.0;
    let info = ShipInfo::new(
        owner,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(6173.0 + x, 300.0, -49000.0),
        Matrix::IDENTITY,
    );
    let ship = space_obj::create(cx, &info).expect("the ship is made");
    (cx.world.find(ship).unwrap(), ship)
}

/// Player `player`'s Content ship (`FContentPlayerShip`) is `ship`, a
/// berth-sized ship in he01's record.
fn as_players_ship(cx: &mut Cx<'_>, player: u32, ship: u32) {
    let mut state = PlayerState::new(cx, player);
    state.ship = Some(PlayerShip::new(
        ship,
        create_id(b"he_fighter"),
        vec![create_id(b"can_use_berths")],
        Some(he01()),
    ));
    let content = &mut cx.logic.content.state;
    content.players.insert(player, state);
    content
        .systems
        .get_mut(&he01())
        .expect("he01's record")
        .add_player(player);
}

/// The station's berths (`GetTerminalInfo` 0, 1, ... of type 1, 2 or 6).
fn berths(cx: &Cx<'_>, station: u32) -> Vec<i32> {
    (0..)
        .map_while(|i| {
            space_obj::terminal_info(cx, station, i)
                .ok()
                .map(|t| (i, t.value))
        })
        .filter(|&(_, t)| matches!(t, 1 | 2 | 6))
        .map(|(i, _)| i)
        .collect()
}

fn docking(cx: &Cx<'_>) -> Vec<(u32, u32, i32)> {
    cx.logic
        .content
        .state
        .dock_queues
        .docking
        .entries
        .iter()
        .map(|e| (e.ship, e.target, e.index))
        .collect()
}

#[test]
fn a_ship_content_does_not_know_is_refused() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("dock refusal", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (_, station) = station(&cx);
    let (_, ship) = fighter(&mut cx, 0.0);
    let _ = sink.returned();
    assert_eq!(space_obj::dock_request(&mut cx, ship, station), 0);
    assert_eq!(sink.returned(), [(ship, 0, 5, -1)]);
    let queues = &cx.logic.content.state.dock_queues;
    assert!(queues.docking.entries.is_empty() && queues.waiting.entries.is_empty());
}

#[test]
fn a_players_ship_docks_through_the_queues() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("dock queues", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (station_h, station) = station(&cx);
    let berths = berths(&cx, station);
    assert!(!berths.is_empty(), "he01_11 has berths");
    let (_, ship) = fighter(&mut cx, 0.0);
    as_players_ship(&mut cx, 1, ship);
    let _ = sink.returned();

    // Granted (4) at the berth nearest the ship.
    assert_eq!(space_obj::dock_request(&mut cx, ship, station), 0);
    let answers = sink.returned();
    let [(answered, 0, 4, port)] = answers[..] else {
        panic!("{answers:?}");
    };
    assert_eq!(answered, ship);
    assert!(berths.contains(&port), "{port} of {berths:?}");
    assert_eq!(docking(&cx), [(ship, station, port)]);
    assert_eq!(
        cx.logic.objects.get(station_h).unwrap().dock_queue,
        [game_logic::dock::DockRequest { port, ship }]
    );

    // The dock completes: the entry goes and the terminal is marked free,
    // which the handler's tail hands over.
    let mut complete = ControllerMessage::DockComplete {
        ship,
        dock: station,
        system: he01(),
    };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut complete);
    assert!(docking(&cx).is_empty());
    let queues = &cx.logic.content.state.dock_queues;
    assert!(queues.terminals.is_empty());
    assert!(
        queues
            .applied
            .iter()
            .any(|t| t.target == station && t.index == port && !t.occupied),
        "{:?}",
        queues.applied
    );
}

#[test]
fn a_waiting_ship_gets_the_next_free_berth() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the waiting list", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (_, station) = station(&cx);
    let berths = berths(&cx, station);
    // Every berth is held by a docking entry of a ship that is not there.
    for (n, &berth) in berths.iter().enumerate() {
        let q = &mut cx.logic.content.state.dock_queues;
        let mut entry = q.new_entry(0.0);
        entry.system = he01();
        entry.ship = 0x7000 + n as u32;
        entry.target = station;
        entry.index = berth;
        q.docking.entries.push(entry);
    }
    let (_, ship) = fighter(&mut cx, 0.0);
    as_players_ship(&mut cx, 1, ship);
    let _ = sink.returned();
    assert_eq!(space_obj::dock_request(&mut cx, ship, station), 0);
    assert_eq!(sink.returned(), [(ship, 0, 3, -1)]);
    let waiting: Vec<u32> = cx
        .logic
        .content
        .state
        .dock_queues
        .waiting
        .entries
        .iter()
        .map(|e| e.ship)
        .collect();
    assert_eq!(waiting, [ship]);

    // The first holder launches: its entry is finished, and the waiting
    // ship is granted the freed berth (4) and leaves the waiting list.
    let mut launched = ControllerMessage::LaunchComplete {
        id: station,
        ship: 0x7000,
    };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut launched);
    assert_eq!(sink.returned(), [(ship, 0, 4, berths[0])]);
    assert!(
        cx.logic
            .content
            .state
            .dock_queues
            .waiting
            .entries
            .is_empty()
    );
    assert!(docking(&cx).contains(&(ship, station, berths[0])));
}

#[test]
fn formation_requests_need_known_and_grouped_ships() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("formation requests", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (_, leader) = fighter(&mut cx, 0.0);
    let (wing_h, wing) = fighter(&mut cx, 100.0);
    as_players_ship(&mut cx, 1, leader);
    let _ = sink.returned();
    // The wingman is no Content ship: -3.
    assert_eq!(
        game_logic::dock::request(&mut cx, wing_h, 1, leader, 0, 0, false),
        0
    );
    assert_eq!(sink.returned(), [(wing, 1, -3, 0)]);
    // Two players who are not grouped: -8.
    as_players_ship(&mut cx, 2, wing);
    assert_eq!(
        game_logic::dock::request(&mut cx, wing_h, 1, leader, 0, 0, false),
        0
    );
    assert_eq!(sink.returned(), [(wing, 1, -8, 0)]);
    let state = &cx.logic.content.state;
    assert!(
        state
            .players
            .values()
            .all(|p| p.ship.as_ref().unwrap().formation_id == u32::MAX)
    );
}
