//! Content's comm system, GCS, through the server (work package P3.6b,
//! decision record 0008 points 7 and 10) over the original data
//! (`outdir/`, he01): the oracle's start-up (Content's GameSupport,
//! SpaceSupport and BaseSupport, then `LoadGame` over `InitialWorld.ini`)
//! and he01 loaded through the server, so `SystemLoaded` gives the record
//! its solar NPCs and the dock hail handler its stations' phantoms.
//! Expected values follow the C++ (`Content/Gcs.cpp`).
//!
//! - A lane ring grants a lane to the leader of a SpacePop formation: the
//!   "granted" conversation (type 2, `TradelaneRequests.cpp:51-60`) is the
//!   leader's "trade lane start", admitted with one roll; its line ends
//!   after its voice duration and 1.25 s of heartbeats, and the
//!   conversation goes.
//! - A SpacePop ship enters the 2500 m phantom of station he01_11: the
//!   station hails it (type 1), the ship submits its data, the station
//!   acknowledges; each line plays when the one before ends. The ship is
//!   then not hailed again by that phantom for 30 s.
//! - A patrol's scan (types 0x11 and 0x14): the announcement, then the
//!   scanned NPC complies and the patrol thanks it.

use std::path::Path;
use std::sync::Arc;

use game_logic::content::gcs::{self, Event, ScanEvent, conversations::Kind};
use game_logic::content::npc_ships::{self, NpcShipParams};
use game_logic::events::{ControllerMessage, NpcShipKey, PhantomKey};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The SpacePop controller id the test's ships claim (no SpacePop is
/// made, so no populator spawns).
const SPACE_POP: ControllerId = ControllerId(0x7777);

/// he01 on the oracle's start-up, multiplayer, `None` without `outdir/`.
fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no comm was spoken");
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
    cx.logic.content.state.space_pop = SPACE_POP;
    Some((world, logic))
}

/// A SpacePop fighter with the voice `pilot_f_leg_m01` at `position`, in
/// formation `formation` at `slot` (-1 for none). Its space object is
/// owned by SpaceSupport (the server refuses an unknown controller); the
/// ship then claims SpacePop's id.
fn spawn(cx: &mut Cx<'_>, position: Vector, formation: u32, slot: u32) -> (NpcShipKey, u32) {
    let params = NpcShipParams {
        controller: cx.logic.content.state.space_support.0,
        archetype: create_id(b"he_fighter"),
        loadout: create_id(b"li_p_he_fighter_loadout01"),
        position,
        voice: create_id(b"pilot_f_leg_m01"),
        formation_id: formation,
        formation_slot: slot,
        ..NpcShipParams::default()
    };
    let (result, key) = npc_ships::create_in_system(cx, create_id(b"he01"), &params, 0);
    assert_eq!(result, 0);
    let key = key.expect("the ship is made");
    npc_ships::get_mut(cx, key)
        .expect("in the arena")
        .controller = SPACE_POP.0;
    let id = npc_ships::space_obj(cx, key);
    assert_ne!(id, 0, "the space object is made");
    (key, id)
}

/// The nickname ids of `names`.
fn ids(names: &[&str]) -> Vec<u32> {
    names.iter().map(|n| create_id(n.as_bytes())).collect()
}

/// The duration of a voice's line (`FindVoiceLineDuration`).
fn line_duration(cx: &Cx<'_>, voice: &str, line: &str) -> f32 {
    game_logic::content::tables::voices::voice_line_duration(
        &cx.logic.content.tables.voice_lines,
        create_id(voice.as_bytes()),
        create_id(line.as_bytes()),
    )
}

/// SpaceSupport's heartbeats of 1 s until the conversation of `at` has
/// played `lines` more lines; the heartbeats it took.
fn heartbeats_until_spoken(cx: &mut Cx<'_>, before: usize) -> u32 {
    let space_support = cx.logic.content.state.space_support;
    for beat in 1..=60 {
        let _ = cx.dispatch(
            space_support,
            &mut ControllerMessage::Heartbeat { elapsed: 1.0 },
        );
        let left: usize = cx
            .logic
            .content
            .state
            .gcs
            .conversations
            .iter()
            .map(|c| c.lines.len())
            .sum();
        if left < before {
            return beat;
        }
    }
    panic!("no line ended within 60 heartbeats");
}

#[test]
fn a_lane_grant_is_the_formation_leaders_lane_start() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let ring = create_id(b"he01_Trade_Lane_Ring_16");
    let end = create_id(b"he01_Trade_Lane_Ring_18");
    let (_, leader) = spawn(&mut cx, Vector::new(3800.0, 0.0, -51800.0), 9, 0);
    let (_, _wingman) = spawn(&mut cx, Vector::new(3850.0, 0.0, -51800.0), 9, 1);
    // The ring is a solar NPC of the record (behaviour and reputation).
    assert!(gcs::find_ship_or_solar(&cx, Some(he01), ring).is_some());

    let mut draws = cx.world.rng().clone();
    game_logic::content::tradelane_requests::handle_request(&mut cx, leader, ring, end, he01);
    // The admission's roll, the line's rand() test and its voice line's
    // pick (one permutation).
    for _ in 0..3 {
        draws.rand();
    }
    assert_eq!(*cx.world.rng(), draws);

    let gcs = &cx.logic.content.state.gcs;
    assert_eq!(gcs.conversations.len(), 1);
    let c = &gcs.conversations[0];
    assert_eq!(c.kind, Kind::Dock);
    assert_eq!(c.lines.len(), 1);
    let line = &c.lines[0];
    assert_eq!((line.speaker, line.listener), (leader, 0));
    assert_eq!(line.mood, 0x4a39);
    assert_eq!(line.texts, ids(&["gcs_dockrequest_tradelanestart_01-"]));
    // Recorded with both objects.
    let recent = &gcs.recent[&2];
    assert_eq!((recent[0].speaker, recent[0].listener), (ring, leader));
    // The line's end is queued after its voice duration and 1.25 s.
    let duration = line_duration(&cx, "pilot_f_leg_m01", "gcs_dockrequest_tradelanestart_01-");
    assert!(duration > 0.0);
    let delayed = cx.logic.content.state.gcs.delayed[0];
    assert_eq!(delayed.delay, (f64::from(duration) + 1.25) as f32);
    assert_eq!(
        (delayed.system, delayed.speaker, delayed.listener),
        (he01, leader, 0)
    );

    let beats = heartbeats_until_spoken(&mut cx, 1);
    assert_eq!(beats, (f64::from(duration) + 1.25).ceil() as u32);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());
    assert!(cx.logic.content.state.gcs.delayed.is_empty());

    // A second grant within 12 s and 10 km is not admitted: no roll.
    let draws = cx.world.rng().clone();
    game_logic::content::tradelane_requests::handle_request(&mut cx, leader, ring, end, he01);
    assert_eq!(*cx.world.rng(), draws);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());
}

#[test]
fn a_station_hails_a_ship_entering_its_phantom() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let station = create_id(b"he01_11");
    // `SystemLoaded` gave the station (its base has a msg_id_prefix) a
    // phantom.
    let phantom = cx.logic.content.state.gcs.dock_hail.phantoms[&he01]
        .iter()
        .find(|&(_, &o)| o == station)
        .map(|(&p, _)| PhantomKey(p))
        .expect("he01_11's phantom");
    let (_, ship) = spawn(
        &mut cx,
        Vector::new(6173.0, 0.0, -48500.0),
        u32::MAX,
        u32::MAX,
    );

    let space_support = cx.logic.content.state.space_support;
    let mut enter = ControllerMessage::PhantomEnter {
        object: ship,
        phantom,
        system: he01,
    };
    let _ = cx.dispatch(space_support, &mut enter);
    let gcs = &cx.logic.content.state.gcs;
    assert_eq!(
        gcs.dock_hail.recent_hails.back(),
        Some(&(Some(phantom), ship))
    );
    assert_eq!(gcs.conversations.len(), 1);
    let c = &gcs.conversations[0];
    assert_eq!(c.kind, Kind::DockHail);
    // The hail, the ship's data and the station's acknowledgement (the
    // nothing guide adds no line).
    let who: Vec<(u32, u32)> = c.lines.iter().map(|l| (l.speaker, l.listener)).collect();
    assert_eq!(who, [(station, ship), (ship, station), (station, ship)]);
    let hail = &c.lines[0].texts;
    let first = hail[0];
    assert!(
        first == create_id(b"gcs_dockhail_dock_incoming+")
            || first == create_id(b"gcs_misc_thisis+"),
        "the hail opens with one of its two shapes"
    );
    assert!(hail.contains(&create_id(b"gcs_refer_base_he01_11_Base-")));
    assert_eq!(c.lines[1].texts[0], create_id(b"gcs_misc_thisis+"));
    assert_eq!(
        c.lines[2].texts.last().copied().map(|t| t != 0),
        Some(true),
        "the acknowledgement asks where the ship is headed"
    );

    // Each line plays once the one before has ended.
    for left in [3, 2, 1] {
        heartbeats_until_spoken(&mut cx, left);
    }
    assert!(cx.logic.content.state.gcs.conversations.is_empty());

    // Within 30 s the phantom does not hail the ship again.
    let _ = cx.dispatch(space_support, &mut enter);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());
}

#[test]
fn a_scan_is_announced_and_complied_with() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");
    let (_, patrol) = spawn(
        &mut cx,
        Vector::new(15000.0, 3000.0, -25000.0),
        u32::MAX,
        u32::MAX,
    );
    let (_, trader) = spawn(
        &mut cx,
        Vector::new(15300.0, 3000.0, -25000.0),
        u32::MAX,
        u32::MAX,
    );
    let scan = |kind| {
        Event::Scan(ScanEvent {
            system: Some(he01),
            kind,
            speaker: patrol,
            listener: trader,
            loot_taker: 0,
            good: 0,
            response: 1,
        })
    };

    assert_eq!(gcs::queue_conversation(&mut cx, &scan(0x11)), 1.0);
    let c = &cx.logic.content.state.gcs.conversations[0];
    assert_eq!(c.kind, Kind::Scan);
    let line = &c.lines[0];
    assert_eq!(
        (line.speaker, line.listener, line.mood),
        (patrol, trader, 0x4a43)
    );
    // "This is" the patrol "to" the trader, then the announcement.
    assert_eq!(line.texts.len(), 11);
    assert_eq!(line.texts[0], create_id(b"gcs_misc_thisis+"));
    assert_eq!(line.texts[1], create_id(b"gcs_refer_faction_player_short"));
    assert_eq!(line.texts[4], create_id(b"gcs_misc_to"));
    let announce = *line.texts.last().unwrap();
    assert!(
        (1..=3).any(|n| announce == create_id(format!("gcs_scan_announce_{n:02}-").as_bytes())),
        "one of the voice's three announcements"
    );
    heartbeats_until_spoken(&mut cx, 1);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());

    // The player jettisoned... an NPC complies, the patrol is happy.
    assert_eq!(gcs::queue_conversation(&mut cx, &scan(0x14)), 1.0);
    let c = &cx.logic.content.state.gcs.conversations[0];
    let who: Vec<(u32, u32)> = c.lines.iter().map(|l| (l.speaker, l.listener)).collect();
    assert_eq!(who, [(trader, patrol), (patrol, trader)]);
    heartbeats_until_spoken(&mut cx, 2);
    heartbeats_until_spoken(&mut cx, 1);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());

    // Type 0x17 says nothing (the C++ calls through null there).
    assert_eq!(gcs::queue_conversation(&mut cx, &scan(0x17)), 0.0);
    assert!(cx.logic.content.state.gcs.conversations.is_empty());
}
