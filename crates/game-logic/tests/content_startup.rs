//! Content's skeleton (work package P3.4a, decision record 0008): the
//! constructors' effects on the bus and on Content's counter, GameSupport's
//! heartbeat and the game time, Content.dll's unload, the create
//! parameters and the take rule.
//!
//! Over the original data (`outdir/`; each test returns early with a notice
//! when it is missing): the shipped `[Initial SP DLLs]` rows create
//! Content's controllers in the C++ order with their ids, each constructor
//! drawing its heartbeat's phase from the generator in creation order.
//!
//! With `OUTPOST_ORACLE_DIR` set to the scenario oracle's output (the data
//! job's `outpost-sim`), [`oracle_start_up_and_heartbeats`] compares, for
//! each scenario's first run:
//!
//! - the controllers the oracle's server made (`session.jsonl`'s
//!   `heartbeats`: list order, id, priority, interval and phase), against
//!   the port's start-up from the oracle's `[Initial MP DLLs]` rows
//!   (GameSupport, SpaceSupport, BaseSupport) on `startup_seed`;
//! - with the dispatch trace (`dispatch.jsonl`, record 0006 point 10), every
//!   `Heartbeat` delivered to a Content controller, by tick, id and elapsed
//!   time, against the port's bus stepped with the scenario's frame;
//! - the `TimerExpired` deliveries to Content's controllers, whose first
//!   word is a value of Content's counter: reported with the timer the
//!   port's counter gives that value.
//!
//! `load_rand.jsonl`'s `controller` phase is reported beside them: it holds
//! the harness's create of its own controller, after Content's
//! controllers drew their phases inside `IServerImpl::Startup`.
//! The summary goes to `rust-content-startup.txt` in the oracle folder.

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::content::mission::{MISSION_PATH_BUFFER, MissionPath};
use game_logic::content::random_mission::RmDataKey;
use game_logic::content::{self, Instance, game_support, population};
use game_logic::events::ControllerMessage;
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::scenario::records::{Json, find_files, read_lines};
use neuron_core::ini::IniReader;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The databases of `outdir/`, `None` when it is missing.
fn game_data() -> Option<Arc<GameData>> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    Some(Arc::new(data))
}

/// A world without data: the constructors read none.
fn bare_world() -> World {
    World::new(Arc::new(GameData::new()), WorldConfig::default())
}

fn create(cx: &mut Cx<'_>, name: &str, params: &CreateParams, priority: Priority) -> ControllerId {
    controller::create(cx, b"Content.dll", name.as_bytes(), params, priority)
}

/// `rand() * (1/32768) * interval`, rounded to the accumulator's `float`
/// (`Server_ControllerSetHeartbeat`), computed here from the generator.
fn phase(rand: i32, interval: f32) -> f32 {
    (f64::from(rand) * (1.0 / 32768.0) * f64::from(interval)) as f32
}

/// The server INI's `[Initial * DLLs]` rows.
fn initial_rows(single_player: bool) -> Option<neuron_server::controller::InitialControllers> {
    let text = fs::read(outdir().join("OutpostFrontier.ini")).ok()?;
    let mut reader = IniReader::new();
    assert!(reader.open_memory(&text));
    Some(controller::read_initial_controllers(
        &mut reader,
        single_player,
    ))
}

#[test]
fn the_counter_hands_out_the_load_time_ids_then_the_routers() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let state = &logic.content.state;
    // SpaceSupport.cpp, FactionProps.cpp, Gcs.cpp: Content.vcxproj's order.
    assert_eq!(state.load_ids.music_timer, 1);
    assert_eq!(state.load_ids.faction_names(), [2, 3, 4]);
    assert_eq!(state.load_ids.dock_hail_timer, 5);
    assert_eq!(state.ids.peek(), 6);
    assert!(state.router.is_none());
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let id = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    assert_ne!(id, ControllerId::BROADCAST);
    let state = &cx.logic.content.state;
    assert_eq!(state.router.map(|r| r.timer_id()), Some(6));
    assert_eq!(state.router.map(|r| r.last_handler_id()), Some(0));
    assert_eq!(state.ids.peek(), 7);
    // A second GameSupport makes a second router from the counter.
    let _ = create(
        &mut cx,
        "gamesupport",
        &CreateParams::None,
        Priority::Highest,
    );
    assert_eq!(cx.logic.content.state.router.map(|r| r.timer_id()), Some(7));
    assert_eq!(cx.logic.content.state.ids.peek(), 8);
}

#[test]
fn the_constructors_draw_their_phases_in_creation_order() {
    let Some(data) = game_data() else {
        println!("outdir/ is missing: no start-up");
        return;
    };
    let Some(rows) = initial_rows(true) else {
        println!("outdir/OutpostFrontier.ini is missing: no start-up");
        return;
    };
    let seed = 0x4f52_4143;
    let mut world = World::new(
        data,
        WorldConfig {
            single_player: true,
            ..WorldConfig::default()
        },
    );
    world.rng().seed(seed);
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let ids: Vec<u32> = rows
        .rows
        .iter()
        .map(|row| {
            controller::create(
                &mut cx,
                &row.dll,
                &row.instance,
                &CreateParams::None,
                row.priority,
            )
            .0
        })
        .collect();
    // GameSupport, SpaceSupport, BaseSupport, StoryLauncher, SpacePop,
    // AISandbox and TestAutomation (no factory: their ids are used up),
    // BasePop.
    assert_eq!(ids, [1, 2, 3, 4, 5, 0, 0, 8]);

    // The draws in creation order: GameSupport (1 s), SpaceSupport (1 s),
    // StoryLauncher (none), SpacePop (3 s), BasePop (none); BaseSupport
    // sets no heartbeat.
    let mut rand = CrtRand::with_seed(seed);
    let expected: Vec<(u32, &str, Priority, f32, f32)> = [
        (1, "GameSupport", Priority::Highest, 1.0),
        (2, "SpaceSupport", Priority::Normal, 1.0),
        (4, "StoryLauncher", Priority::BelowNormal, -1.0),
        (5, "SpacePop", Priority::Lowest, 3.0),
        (8, "BasePop", Priority::Lowest, -1.0),
    ]
    .into_iter()
    .map(|(id, name, priority, interval)| {
        (id, name, priority, interval, phase(rand.rand(), interval))
    })
    .collect();
    let find = |id: u32| expected.iter().find(|e| e.0 == id).copied();

    // The list: by priority, each ahead of the equals made before it.
    let listed: Vec<(u32, &str, Priority, f32, f32)> = cx
        .logic
        .bus
        .active()
        .iter()
        .map(|n| {
            (
                n.id.0,
                n.kind.expect("constructed").name(),
                n.priority,
                n.heartbeat_period,
                n.heartbeat_accum,
            )
        })
        .collect();
    let order: Vec<(u32, &str)> = listed.iter().map(|l| (l.0, l.1)).collect();
    assert_eq!(
        order,
        [
            (1, "GameSupport"),
            (3, "BaseSupport"),
            (2, "SpaceSupport"),
            (4, "StoryLauncher"),
            (8, "BasePop"),
            (5, "SpacePop"),
        ]
    );
    for l in &listed {
        match find(l.0) {
            Some(e) => {
                assert_eq!(
                    (l.2, l.3.to_bits(), l.4.to_bits()),
                    (e.2, e.3.to_bits(), e.4.to_bits()),
                    "{}",
                    l.1
                );
            }
            // BaseSupport: the bus's defaults.
            None => assert_eq!((l.3, l.4), (-1.0, 0.0), "{}", l.1),
        }
    }
    // Nothing else drew.
    assert_eq!(cx.world.rng().rand(), rand.rand());

    let state = &cx.logic.content.state;
    assert_eq!(state.space_support, ControllerId(2));
    assert_eq!(state.space_pop, ControllerId(5));
    assert_eq!(state.base_pop, Some(ControllerId(8)));
    assert_eq!(state.router.map(|r| r.timer_id()), Some(6));
    assert_eq!(state.ids.peek(), 7);
    let kinds: Vec<&str> = cx
        .logic
        .content
        .instances
        .values()
        .map(|i| match i {
            Instance::GameSupport => "GameSupport",
            Instance::SpaceSupport => "SpaceSupport",
            Instance::BaseSupport => "BaseSupport",
            Instance::SpacePop => "SpacePop",
            Instance::BasePop(_) => "BasePop",
            Instance::StoryLauncher => "StoryLauncher",
            _ => "other",
        })
        .collect();
    assert_eq!(
        kinds,
        [
            "GameSupport",
            "SpaceSupport",
            "BaseSupport",
            "StoryLauncher",
            "SpacePop",
            "BasePop"
        ]
    );
}

#[test]
fn game_supports_heartbeat_adds_up_the_game_time() {
    let mut world = bare_world();
    world.rng().seed(7);
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    logic.deliver_hook = Some(|_, _, kind, message| {
        if let (ControllerKind::GameSupport, ControllerMessage::Heartbeat { elapsed }) =
            (kind, &*message)
        {
            BEATS.with(|b| b.borrow_mut().push(*elapsed));
        }
    });
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let id = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let start = cx.logic.bus.find(id).unwrap().heartbeat_accum;
    assert_eq!(
        start.to_bits(),
        phase(CrtRand::with_seed(7).rand(), 1.0).to_bits()
    );
    let dt = 1.0f32 / 30.0;
    let frames = 200;
    // The bus's accumulator on the T11 model, beside the bus.
    let mut accum = start;
    let mut expected = Vec::new();
    for _ in 0..frames {
        controller::tick(&mut cx, dt);
        let sum = f64::from(dt) + f64::from(accum);
        accum = sum as f32;
        if sum >= 1.0 {
            let wrap = sum.floor() as f32;
            expected.push(wrap);
            accum = (f64::from(accum) - f64::from(wrap)) as f32;
        }
    }
    let beats = BEATS.with(|b| b.borrow().clone());
    assert_eq!(beats, expected);
    assert!(beats.len() >= 6, "{beats:?}");
    let total: f64 = beats.iter().map(|&b| f64::from(b)).sum();
    assert_eq!(game_support::game_time(&cx), total);
    assert_eq!(cx.logic.content.state.game_time, beats.len() as f64);
}

thread_local! {
    static BEATS: std::cell::RefCell<Vec<f32>> = const { std::cell::RefCell::new(Vec::new()) };
}

#[test]
fn the_time_distance_keeps_the_no_time_value() {
    assert_eq!(game_support::time_distance(3.0, 5.5), 2.5);
    assert_eq!(game_support::time_distance(5.5, 3.0), 2.5);
    assert_eq!(game_support::time_distance(-1.0, 3.0), -1.0);
    assert_eq!(game_support::time_distance(3.0, -1.0), -1.0);
    assert_eq!(game_support::time_distance(-2.0, 3.0), 5.0);
}

#[test]
fn the_last_content_controller_unloads_the_dll() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let game = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let space = create(
        &mut cx,
        "SpaceSupport",
        &CreateParams::None,
        Priority::Normal,
    );
    controller::tick(&mut cx, 1.5);
    assert!(cx.logic.content.state.game_time > 0.0);

    // GameSupport's destructor: no router, no tables; Content stays loaded
    // while SpaceSupport lives.
    controller::destroy(&mut cx, game);
    controller::tick(&mut cx, 0.0);
    let state = &cx.logic.content.state;
    assert!(state.router.is_none());
    assert_eq!(state.ids.peek(), 7);
    assert_eq!(state.space_support, space);
    assert!(state.game_time > 0.0);
    assert_eq!(cx.logic.content.instances.len(), 1);

    // The last one: the DLL unloads, and the next create loads it afresh.
    controller::destroy(&mut cx, space);
    controller::tick(&mut cx, 0.0);
    let state = &cx.logic.content.state;
    assert_eq!(state.ids.peek(), 6);
    assert_eq!(state.game_time, 0.0);
    assert_eq!(state.space_support, ControllerId::BROADCAST);
    assert!(cx.logic.content.instances.is_empty());
    let again = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    assert_ne!(again, game);
    assert_eq!(cx.logic.content.state.router.map(|r| r.timer_id()), Some(6));
}

#[test]
fn base_pop_is_the_last_one_made_and_reads_as_none_once_torn_down() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let _keep = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let first = create(&mut cx, "BasePop", &CreateParams::None, Priority::Lowest);
    assert_eq!(cx.logic.content.state.base_pop, Some(first));
    // A second BasePop (the C++'s breakpoint, D771) becomes the instance.
    let second = create(&mut cx, "BasePop", &CreateParams::None, Priority::Lowest);
    assert_eq!(cx.logic.content.state.base_pop, Some(second));
    if let Some(Instance::BasePop(pop)) = cx.logic.content.instance_mut(second) {
        pop.player_npcs.insert(3, vec![1]);
    }
    assert!(population::base_pop_has_player(&cx, 3));
    assert!(!population::base_pop_has_player(&cx, 4));
    // Torn down, it reads as none (D772); the global keeps its id.
    controller::destroy(&mut cx, second);
    controller::tick(&mut cx, 0.0);
    assert_eq!(cx.logic.content.state.base_pop, Some(second));
    assert!(population::base_pop(&cx).is_none());
    assert!(!population::base_pop_has_player(&cx, 3));
}

#[test]
fn the_create_parameters_reach_their_instances() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let story = create(
        &mut cx,
        "Story",
        &CreateParams::Story { player: 3 },
        Priority::Normal,
    );
    let mission = create(
        &mut cx,
        "Mission_04",
        &CreateParams::StoryMission { player: 3, story },
        Priority::AboveNormal,
    );
    let generic = create(
        &mut cx,
        "GenericMission",
        &CreateParams::GenericMission {
            player: 3,
            story,
            path: MissionPath::new(b"missions\\generic\\m.ini"),
        },
        Priority::Normal,
    );
    let random = create(
        &mut cx,
        "DestroyMission",
        &CreateParams::RandomMission(RmDataKey(9)),
        Priority::Normal,
    );
    // Parameters of another form read as zeros (D770).
    let bare_story = create(&mut cx, "Story", &CreateParams::None, Priority::Normal);
    let bare_mission = create(
        &mut cx,
        "Mission_01a",
        &CreateParams::Story { player: 5 },
        Priority::Normal,
    );
    let bare_random = create(
        &mut cx,
        "DestroyMission",
        &CreateParams::None,
        Priority::Normal,
    );

    let content = &cx.logic.content;
    assert!(matches!(content.instance(story), Some(Instance::Story(s)) if s.player == 3));
    match content.instance(mission) {
        Some(Instance::Mission(m)) => {
            assert_eq!(
                (m.kind, m.player, m.story),
                (ControllerKind::Mission(4), 3, story)
            );
            assert!(m.path.as_bytes().is_empty());
        }
        other => panic!("{other:?}"),
    }
    match content.instance(generic) {
        Some(Instance::Mission(m)) => {
            assert_eq!(m.kind, ControllerKind::GenericMission);
            assert_eq!(m.path.as_bytes(), b"missions\\generic\\m.ini");
        }
        other => panic!("{other:?}"),
    }
    assert!(
        matches!(content.instance(random), Some(Instance::RandomMission(r)) if r.data == Some(RmDataKey(9)))
    );
    assert!(matches!(content.instance(bare_story), Some(Instance::Story(s)) if s.player == 0));
    assert!(matches!(
        content.instance(bare_mission),
        Some(Instance::Mission(m)) if m.player == 5 && m.story == ControllerId::BROADCAST
    ));
    assert!(
        matches!(content.instance(bare_random), Some(Instance::RandomMission(r)) if r.data.is_none())
    );

    // The path keeps the buffer's bound and stops at a NUL (D773).
    let long = vec![b'a'; 100];
    assert_eq!(
        MissionPath::new(&long).as_bytes().len(),
        MISSION_PATH_BUFFER - 1
    );
    assert_eq!(MissionPath::new(b"ab\0cd").as_bytes(), b"ab");
}

#[test]
fn a_take_around_a_call_that_does_not_dispatch_puts_the_item_back() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let pop = create(&mut cx, "BasePop", &CreateParams::None, Priority::Lowest);
    let found = content::with_instance_taken(&mut cx, pop, |cx, instance| {
        // Out of its slot during the call.
        assert!(cx.logic.content.instance(pop).is_none());
        matches!(instance, Instance::BasePop(_))
    });
    assert_eq!(found, Some(true));
    assert!(cx.logic.content.instance(pop).is_some());
    assert_eq!(
        content::with_instance_taken(&mut cx, ControllerId(0x77), |_, _| ()),
        None
    );
    // Deliveries are counted (debug builds).
    let before = cx.logic.content.deliveries();
    let mut message = ControllerMessage::PlayerLogin { player: 1 };
    cx.dispatch(ControllerId::BROADCAST, &mut message).unwrap();
    if cfg!(debug_assertions) {
        assert_eq!(cx.logic.content.deliveries(), before + 1);
    }
}

#[cfg(debug_assertions)]
#[test]
#[should_panic(expected = "while a Content item was out of its slot")]
fn a_take_around_a_dispatch_is_caught() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let game = create(
        &mut cx,
        "GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let take = game_logic::content::NoDispatch::begin(&cx.logic.content);
    let mut message = ControllerMessage::Heartbeat { elapsed: 1.0 };
    cx.dispatch(game, &mut message).unwrap();
    take.end(&cx.logic.content);
}

#[test]
fn space_support_answers_the_lane_requests() {
    let mut world = bare_world();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let space = create(
        &mut cx,
        "SpaceSupport",
        &CreateParams::None,
        Priority::Normal,
    );
    // A ring the world does not have is refused; the request reaches
    // SpaceSupport's trade-lane case without a world object to answer.
    let mut message = ControllerMessage::TradelaneRequest {
        ship: 0x1234,
        ring: 0x5678,
        end_ring: 0,
        system: 0,
    };
    assert_eq!(cx.dispatch(space, &mut message), Ok(()));
    assert_eq!(cx.logic.content.state.space_support, space);
}

// ---------------------------------------------------------------------------
// The oracle's start-up
// ---------------------------------------------------------------------------

/// A Content controller as the oracle's `heartbeats` record lists it.
#[derive(Clone, Debug, PartialEq)]
struct Listed {
    flid: u32,
    priority: u32,
    interval: u32,
    phase: u32,
    deferred: bool,
}

fn listed(controller: &Json) -> Option<(String, Listed)> {
    Some((
        controller.get("module")?.as_str()?.to_owned(),
        Listed {
            flid: controller.get("flid")?.as_u32()?,
            priority: controller.get("priority")?.as_u32()?,
            interval: controller.get("interval")?.as_f32()?.to_bits(),
            phase: controller.get("phase")?.as_f32()?.to_bits(),
            deferred: controller.get("deferred")?.as_bool()?,
        },
    ))
}

/// One `deliver` record of `dispatch.jsonl`.
struct Delivery {
    tick: u32,
    flid: u32,
    msg: u32,
    elapsed: Option<f32>,
    p1: Option<u32>,
    p2: Option<u32>,
}

fn delivery(record: &Json) -> Option<Delivery> {
    if record.get("rec")?.as_str()? != "deliver" {
        return None;
    }
    Some(Delivery {
        tick: record.get("tick")?.as_u32()?,
        flid: record.get("flid")?.as_u32()?,
        msg: record.get("msg")?.as_u32()?,
        elapsed: record.get("elapsed").and_then(Json::as_f32),
        p1: record.get("p1").and_then(Json::as_u32),
        p2: record.get("p2").and_then(Json::as_u32),
    })
}

/// The oracle's server INI rows: `[Initial MP DLLs]` with only Content's
/// GameSupport, SpaceSupport and BaseSupport (`WriteServerIni`,
/// `CppSource/Oracle/oracle_sim.cpp`). Every row names its priority, so
/// leaving rows out after reading them is the same as reading the INI
/// with them commented out.
fn oracle_rows() -> Option<Vec<neuron_server::controller::InitialController>> {
    let rows = initial_rows(false)?;
    Some(
        rows.rows
            .into_iter()
            .filter(|row| {
                let name = String::from_utf8_lossy(&row.instance).to_ascii_lowercase();
                ["gamesupport", "spacesupport", "basesupport"].contains(&name.as_str())
            })
            .collect(),
    )
}

/// A heartbeat delivery as `tick flid elapsed`.
fn beat(b: Option<&(u32, u32, u32)>) -> String {
    b.map_or_else(
        || "none".to_owned(),
        |&(tick, flid, elapsed)| format!("tick {tick} to {flid}, {}", f32::from_bits(elapsed)),
    )
}

/// The name the port's counter gives a timer's first word.
fn timer_name(logic: &Logic, value: u32) -> String {
    let state = &logic.content.state;
    let ids = &state.load_ids;
    if value == ids.music_timer {
        "the music timer".to_owned()
    } else if value == ids.faction_name_pilot {
        "the pilot-name release".to_owned()
    } else if value == ids.faction_name_large_ship {
        "the large-ship-name release".to_owned()
    } else if value == ids.faction_name_formation {
        "the formation-name release".to_owned()
    } else if value == ids.dock_hail_timer {
        "the dock-hail timer".to_owned()
    } else if state.router.is_some_and(|r| r.timer_id() == value) {
        "the router's timer".to_owned()
    } else if value >= state.ids.peek() {
        "a run-time id (NPC ships, nags, NN paths; P3.5 and P3.6)".to_owned()
    } else {
        "no id of the port's counter".to_owned()
    }
}

#[test]
fn oracle_start_up_and_heartbeats() {
    let Some(dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!("OUTPOST_ORACLE_DIR is not set: no comparison with the oracle's start-up");
        return;
    };
    let Some(data) = game_data() else {
        println!("outdir/ is missing: no comparison with the oracle's start-up");
        return;
    };
    let Some(rows) = oracle_rows() else {
        println!("outdir/OutpostFrontier.ini is missing: no comparison");
        return;
    };
    let mut summary = String::new();
    let mut failures = Vec::new();
    let mut scenarios = 0;
    let mut entries: Vec<PathBuf> = fs::read_dir(&dir)
        .map(|d| {
            d.flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect()
        })
        .unwrap_or_default();
    entries.sort();
    for scenario_dir in entries {
        let name = scenario_dir
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let Some(session) = find_files(&scenario_dir, "session.jsonl")
            .into_iter()
            .next()
        else {
            continue;
        };
        let run_dir = session.parent().map(Path::to_path_buf).unwrap_or_default();
        let records = read_lines(&session);
        let rec = |kind: &str| {
            records
                .iter()
                .find(|r| r.get("rec").and_then(Json::as_str) == Some(kind))
        };
        let (Some(start), Some(server), Some(beats)) =
            (rec("start"), rec("server"), rec("heartbeats"))
        else {
            let _ = writeln!(summary, "{name}: no start, server or heartbeats record");
            continue;
        };
        let (Some(seed), Some(frame), Some(ticks)) = (
            server.get("startup_seed").and_then(Json::as_u32),
            start.get("frame").and_then(Json::as_f64),
            start.get("ticks").and_then(Json::as_u32),
        ) else {
            let _ = writeln!(summary, "{name}: no startup_seed, frame or ticks");
            continue;
        };
        scenarios += 1;
        let theirs: Vec<Listed> = beats
            .get("controllers")
            .and_then(Json::as_array)
            .unwrap_or_default()
            .iter()
            .filter_map(listed)
            .filter(|(module, _)| module.eq_ignore_ascii_case("Content.dll"))
            .map(|(_, l)| l)
            .collect();

        // The port's start-up on the same seed.
        let mut world = World::new(data.clone(), WorldConfig::default());
        world.rng().seed(seed);
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        for row in &rows {
            let _ = controller::create(
                &mut cx,
                &row.dll,
                &row.instance,
                &CreateParams::None,
                row.priority,
            );
        }
        let content_ids: Vec<u32> = cx.logic.bus.active().iter().map(|n| n.id.0).collect();
        let ours: Vec<Listed> = cx
            .logic
            .bus
            .active()
            .iter()
            .map(|n| Listed {
                flid: n.id.0,
                priority: n.priority.value(),
                interval: n.heartbeat_period.to_bits(),
                phase: n.heartbeat_accum.to_bits(),
                deferred: false,
            })
            .collect();
        let same = ours == theirs;
        let _ = writeln!(
            summary,
            "{name}: startup_seed {seed}, Content's controllers {}",
            if same { "equal" } else { "DIFFER" }
        );
        for (label, list) in [("oracle", &theirs), ("port", &ours)] {
            for l in list {
                let _ = writeln!(
                    summary,
                    "  {label}: flid {} priority {} interval {} phase {} ({:08x}){}",
                    l.flid,
                    l.priority,
                    f32::from_bits(l.interval),
                    f32::from_bits(l.phase),
                    l.phase,
                    if l.deferred { " deferred" } else { "" }
                );
            }
        }
        if !same {
            failures.push(format!("{name}: the start-up's controllers"));
        }

        // load_rand.jsonl's `controller` phase: the harness's own create.
        if let Some(file) = find_files(&run_dir, "load_rand.jsonl").into_iter().next() {
            for record in read_lines(&file) {
                if record.get("rec").and_then(Json::as_str) == Some("phase")
                    && record.get("phase").and_then(Json::as_str) == Some("controller")
                {
                    let _ = writeln!(
                        summary,
                        "  load_rand controller phase: first {} count {} recorded {}",
                        record.get("first").and_then(Json::as_u32).unwrap_or(0),
                        record.get("count").and_then(Json::as_u32).unwrap_or(0),
                        record.get("recorded").and_then(Json::as_u32).unwrap_or(0)
                    );
                }
            }
        }

        // The dispatch trace.
        let Some(trace) = find_files(&run_dir, "dispatch.jsonl").into_iter().next() else {
            let _ = writeln!(summary, "  no dispatch.jsonl");
            continue;
        };
        let deliveries: Vec<Delivery> = read_lines(&trace).iter().filter_map(delivery).collect();
        let their_beats: Vec<(u32, u32, u32)> = deliveries
            .iter()
            .filter(|d| d.msg == 0x20 && content_ids.contains(&d.flid))
            .map(|d| (d.tick, d.flid, d.elapsed.map_or(0, f32::to_bits)))
            .collect();
        let mut our_beats = Vec::new();
        TRACE.with(|t| *t.borrow_mut() = Vec::new());
        cx.logic.deliver_hook = Some(|_, id, _, message| {
            if let ControllerMessage::Heartbeat { elapsed } = *message {
                TRACE.with(|t| t.borrow_mut().push((id.0, elapsed.to_bits())));
            }
        });
        let dt = frame as f32;
        for tick in 1..=ticks {
            controller::tick(&mut cx, dt);
            controller::fire_timers(&mut cx, dt);
            for (flid, elapsed) in TRACE.with(|t| std::mem::take(&mut *t.borrow_mut())) {
                our_beats.push((tick, flid, elapsed));
            }
        }
        let first_difference = their_beats
            .iter()
            .zip(&our_beats)
            .position(|(a, b)| a != b)
            .or_else(|| {
                (their_beats.len() != our_beats.len())
                    .then(|| their_beats.len().min(our_beats.len()))
            });
        let _ = writeln!(
            summary,
            "  heartbeats to Content: oracle {}, port {}{}",
            their_beats.len(),
            our_beats.len(),
            match first_difference {
                None => ", equal".to_owned(),
                Some(i) => format!(
                    ", first difference at {i}: oracle {}, port {}",
                    beat(their_beats.get(i)),
                    beat(our_beats.get(i))
                ),
            }
        );
        if first_difference.is_some() {
            failures.push(format!("{name}: the heartbeats"));
        }
        let mut timers = 0;
        for d in deliveries
            .iter()
            .filter(|d| d.msg == 0x3e && content_ids.contains(&d.flid))
        {
            timers += 1;
            if timers <= 20 {
                let p1 = d.p1.unwrap_or(0);
                let _ = writeln!(
                    summary,
                    "  TimerExpired at tick {} to {}: {{{p1}, {}}}, {}",
                    d.tick,
                    d.flid,
                    d.p2.unwrap_or(0),
                    timer_name(cx.logic, p1)
                );
            }
        }
        let _ = writeln!(summary, "  TimerExpired to Content: {timers}");
    }
    let _ = writeln!(
        summary,
        "{scenarios} scenarios; {}",
        if failures.is_empty() {
            "no difference".to_owned()
        } else {
            failures.join("; ")
        }
    );
    print!("{summary}");
    let _ = fs::write(dir.join("rust-content-startup.txt"), &summary);
    assert!(failures.is_empty(), "{failures:?}");
}

thread_local! {
    static TRACE: std::cell::RefCell<Vec<(u32, u32)>> = const { std::cell::RefCell::new(Vec::new()) };
}
