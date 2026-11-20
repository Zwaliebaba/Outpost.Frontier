//! The run-time base database over `outdir/`: a session asks for a base
//! and a room, news and an NPC on a placement are made and destroyed, and
//! a player's own character enters and leaves, with what reaches the
//! client sink.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::bases::{self, HandlePool};
use game_logic::server::{ClientSink, Cx, Logic};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::ToClient;
use game_sim::protocol::base_props::{CharacterProperties, NewsBroadcastProperties};
use game_sim::protocol::costume::Costume;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

fn kinds(sink: &Shared) -> Vec<(u32, String)> {
    std::mem::take(&mut *sink.0.borrow_mut())
        .into_iter()
        .map(|(c, m)| {
            let name = match m {
                ToClient::Server(s) => format!("{s:?}"),
                ToClient::Common(c) => format!("{c:?}"),
            };
            (c, name.split(['(', ' ', '{']).next().unwrap().to_owned())
        })
        .collect()
}

#[test]
fn handle_pools_wrap_after_the_last_given() {
    let mut pool = HandlePool::default();
    assert_eq!(pool.allocate(), 2, "the walk starts after position 0");
    assert_eq!(pool.allocate_index(), 2);
    pool.set(1, false);
    assert_eq!(pool.allocate_index(), 3);
}

#[test]
fn bases_rooms_and_their_records() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no bases");
        return;
    };
    let source = Host::new(root.join("data"));
    let mut data = GameData::new();
    data.load(&source, &config, None);
    // A base with a room that has placements.
    let base = create_id(b"he01_01_base");
    let room = data.bases.base(base).expect("base").start_room;
    let mut world = World::new(
        Arc::new(data),
        WorldConfig {
            single_player: false,
            ..WorldConfig::default()
        },
    );
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, true);
    logic.players.get_mut(1).unwrap().client_id = 1;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    bases::load_base(&mut cx, &source, base);
    let placements = cx
        .logic
        .bases
        .as_ref()
        .and_then(|db| db.room(room))
        .and_then(|r| r.file.as_ref())
        .map_or(0, |f| f.placements.len());
    assert!(placements > 0, "the start room has placements");

    bases::base_info(&mut cx, 1, base, true);
    assert_eq!(
        kinds(&sink),
        [
            (1, "SetStartRoom".to_owned()),
            (1, "GfCompleteMissionComputerList".to_owned()),
            (1, "GfCompleteNewsBroadcastList".to_owned()),
        ]
    );
    bases::base_info(&mut cx, 1, base, true);
    assert!(kinds(&sink).is_empty(), "already in the base");

    // News for every session reaches the session in the base.
    let news = NewsBroadcastProperties {
        base_id: base,
        ..NewsBroadcastProperties::default()
    };
    let handle = bases::create_news_broadcast(&mut cx, &news, 0);
    assert_ne!(handle, u32::MAX);
    assert_eq!(kinds(&sink), [(1, "GfUpdateNewsBroadcast".to_owned())]);

    bases::location_info(&mut cx, 1, room, true);
    assert_eq!(
        kinds(&sink),
        [
            (1, "GfCompleteCharList".to_owned()),
            (1, "GfCompleteScriptBehaviorList".to_owned()),
            (1, "GfCompleteAmbientScriptList".to_owned()),
        ]
    );
    // An NPC on the room's first placement.
    let placement = cx
        .logic
        .bases
        .as_ref()
        .unwrap()
        .room(room)
        .unwrap()
        .file
        .as_ref()
        .unwrap()
        .placements[0]
        .clone();
    let npc = CharacterProperties {
        room_id: room,
        name_ansi: Some(placement.name.clone()),
        costume: Costume::default(),
        ..CharacterProperties::default()
    };
    let key = bases::create_character(&mut cx, &npc, 0);
    assert!(key > 0);
    assert_eq!(kinds(&sink), [(1, "GfUpdateChar".to_owned())]);
    assert_eq!(
        bases::character_on_placement(&mut cx, room, 0, 0),
        key as u32
    );
    let record = &cx
        .logic
        .bases
        .as_ref()
        .unwrap()
        .room(room)
        .unwrap()
        .characters[0];
    assert_eq!(record.props.posture, placement.posture as u32);
    bases::destroy_character(&mut cx, key as u32);
    assert_eq!(kinds(&sink), [(1, "GfDestroyCharacter".to_owned())]);

    // The player's own character.
    bases::enter_location(&mut cx, 1, room);
    assert_eq!(kinds(&sink), [(1, "GfUpdateChar".to_owned())]);
    bases::exit_location(&mut cx, 1, room);
    assert_eq!(kinds(&sink), [(1, "GfDestroyCharacter".to_owned())]);

    bases::destroy_news_broadcast(&mut cx, handle);
    assert_eq!(kinds(&sink), [(1, "GfDestroyNewsBroadcast".to_owned())]);
    bases::destroy_news_broadcast(&mut cx, handle);
    assert!(kinds(&sink).is_empty());
    assert_eq!(cx.logic.reports.0.len(), 2, "two not-found reports");
}
