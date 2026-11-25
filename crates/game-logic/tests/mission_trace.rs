//! The Tier B mission trace (work package P3.5c1, decision record 0008
//! question 9 and its Verification table): the trigger engine of
//! `game_logic::content::mission::engine` running `Mission_01a`'s opening,
//! against `Content.dll`'s `OracleDumpMissionTrace`
//! (`CppSource/Content/oracle_mission_trace.cpp`, session
//! `CppSource/Oracle/mission_trace.txt`).
//!
//! Both sides start a multiplayer server, register player 7 in
//! `RELAYSEVEN_system` with no ship, make `Mission_01a` for it on the
//! harness's seed with a controller id no controller has (so the bus
//! delivers it nothing), and hand it the session of [`STEPS`]: `MissionStart`
//! at tick 0, then at every tick its steps and a heartbeat of 1/30 s, up to
//! [`TICKS`]. The trace (`mission.jsonl`, the records of
//! `game_logic::content::mission::trace`) is compared exactly, record by
//! record, to the first difference (question 9: the dispatch order to the
//! first difference; nothing in the session has a position to judge within
//! an envelope). The first difference is reported with the records around
//! it, as the scenario judge reports a first divergence.
//!
//! - `data_dump_mission_trace` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   against `mission_trace/mission.jsonl`; summary
//!   `rust-mission-trace.txt`, the port's trace `rust-mission.jsonl`.
//! - `the_opening_runs_on_outdir`: the session on `outdir/`, its first
//!   triggers checked by hand; it returns early without the data.
//!
//! Nothing these tests write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::mission::trace::TraceRecord;
use game_logic::content::mission::{self, engine};
use game_logic::content::players::PlayerState;
use game_logic::content::{Instance, mission::MissionScript};
use game_logic::events::{CommResult, ControllerMessage};
use game_logic::selection;
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// `OracleTraceSeed`, the harness's default seed.
const SEED: u32 = 0x4f52_4143;
/// `OracleTracePlayerId`.
const PLAYER: u32 = 7;
/// `OracleTraceController`: no controller has it.
const CONTROLLER: ControllerId = ControllerId(0x00ff_0001);
/// `OracleTraceSystem`.
const SYSTEM: &[u8] = b"RELAYSEVEN_system";
/// `OracleTraceTicks`: 30 s of 1/30 s heartbeats.
const TICKS: u32 = 900;

/// One step of the session (`FOracleTraceStep`).
#[derive(Clone, Copy)]
enum Step {
    /// `MissionStart`.
    MissionStart,
    /// `SpaceEnter {system, player, 0, 0}`.
    SpaceEnter,
    /// `CommComplete {0, system, 0, line, 0}`.
    CommComplete(&'static [u8]),
    /// `SpaceExit {system, player}`.
    SpaceExit,
    /// `BaseEnter {base, player}`.
    BaseEnter(&'static [u8]),
}

/// `OracleTraceSteps`: the session, by tick, in order within a tick (before
/// the tick's heartbeat).
const STEPS: [(u32, Step); 7] = [
    (0, Step::MissionStart),
    (2, Step::SpaceEnter),
    (200, Step::CommComplete(b"DX_M01_0000_FREEPORT_7_BRIDGE")),
    (260, Step::CommComplete(b"DX_M01_0005_Samura_SA_14")),
    (320, Step::CommComplete(b"DX_M01_0010_FREEPORT_7_BRIDGE")),
    (600, Step::SpaceExit),
    (620, Step::BaseEnter(b"he01_01_Base")),
];

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer server over a data root, started as the oracle starts it
/// (as `mission_saves.rs` does).
fn started(exe: &Path, data_dir: &Path) -> Option<(World, Logic)> {
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(data_dir.to_path_buf()));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        ("GameSupport", Priority::Highest),
        ("SpaceSupport", Priority::Normal),
        ("BaseSupport", Priority::Normal),
    ] {
        let id = controller::create(
            &mut cx,
            b"Content.dll",
            name.as_bytes(),
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{name}");
    }
    let text = Host::new(data_dir)
        .read(b"initialworld.ini")
        .unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    selection::load_world_state(&mut cx, &mut reader);
    Some((world, logic))
}

/// The session's trace on a started server.
fn run_session(cx: &mut Cx<'_>) -> Vec<TraceRecord> {
    // RegisterOracleTracePlayer.
    let system = api::misc::system_id(cx, SYSTEM).ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = system.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
    let system = system.unwrap_or(0);
    let base = |cx: &mut Cx<'_>, name: &[u8]| api::misc::base_id(cx, name).unwrap_or(0);
    // MakeOracleTraceMission.
    cx.logic.content.state.mission_dialogs.clear();
    cx.world.rng().seed(SEED);
    let script = mission::construct_script(
        cx,
        CONTROLLER,
        ControllerKind::Mission01a,
        &CreateParams::StoryMission {
            player: PLAYER,
            story: ControllerId::BROADCAST,
        },
    );
    cx.logic
        .content
        .instances
        .insert(CONTROLLER, Instance::Mission(Box::new(script)));
    engine::start_trace(cx, CONTROLLER);
    let dt = 1.0f32 / 30.0;
    for tick in 0..=TICKS {
        set_tick(cx, tick);
        for &(_, step) in STEPS.iter().filter(|(t, _)| *t == tick) {
            let mut message = match step {
                Step::MissionStart => ControllerMessage::MissionStart,
                Step::SpaceEnter => ControllerMessage::SpaceEnter {
                    system,
                    player: PLAYER,
                    launch_object: 0,
                    launch_kind: 0,
                },
                Step::CommComplete(line) => ControllerMessage::CommComplete {
                    listener: 0,
                    system,
                    speaker: 0,
                    line: create_id(line),
                    result: CommResult::Success,
                },
                Step::SpaceExit => ControllerMessage::SpaceExit {
                    system,
                    player: PLAYER,
                },
                Step::BaseEnter(name) => ControllerMessage::BaseEnter {
                    base: base(cx, name),
                    player: PLAYER,
                },
            };
            deliver(cx, tick, &mut message);
        }
        if tick > 0 {
            deliver(cx, tick, &mut ControllerMessage::Heartbeat { elapsed: dt });
        }
    }
    engine::finish_trace(cx, CONTROLLER);
    let records = script_of(cx)
        .and_then(|s| s.trace.as_ref())
        .map(|t| t.records.clone())
        .unwrap_or_default();
    cx.logic.content.instances.remove(&CONTROLLER);
    cx.logic.content.state.mission_dialogs.clear();
    cx.logic.content.state.players.remove(&PLAYER);
    records
}

fn script_of<'c>(cx: &'c Cx<'_>) -> Option<&'c MissionScript> {
    match cx.logic.content.instances.get(&CONTROLLER) {
        Some(Instance::Mission(s)) => Some(s),
        _ => None,
    }
}

fn set_tick(cx: &mut Cx<'_>, tick: u32) {
    if let Some(Instance::Mission(s)) = cx.logic.content.instances.get_mut(&CONTROLLER)
        && let Some(t) = s.trace.as_mut()
    {
        t.tick = tick;
    }
}

/// The export's `message` record, then `IDLL::HandleMessage`.
fn deliver(cx: &mut Cx<'_>, tick: u32, message: &mut ControllerMessage<'_>) {
    let msg = message.id();
    if let Some(Instance::Mission(s)) = cx.logic.content.instances.get_mut(&CONTROLLER)
        && let Some(t) = s.trace.as_mut()
    {
        t.records.push(TraceRecord::Message { tick, msg });
    }
    engine::handle_message(cx, CONTROLLER, message);
}

/// Records shown on each side of a first difference.
const REPORT_WINDOW: usize = 2;

/// The comparison: `None` when equal, else the summary lines of the first
/// difference.
fn first_difference(rust: &[String], oracle: &[String]) -> Option<Vec<String>> {
    let at = (0..rust.len().max(oracle.len())).find(|&i| rust.get(i) != oracle.get(i))?;
    let mut lines = vec![format!(
        "mission_trace: first divergence at record {at} of rust {} / oracle {}",
        rust.len(),
        oracle.len()
    )];
    let from = at.saturating_sub(REPORT_WINDOW);
    for i in from..=at + REPORT_WINDOW {
        let r = rust.get(i).map_or("-", String::as_str);
        let o = oracle.get(i).map_or("-", String::as_str);
        let mark = if i == at { ">" } else { " " };
        lines.push(format!("{mark} {i}: rust   {r}"));
        lines.push(format!("{mark} {i}: oracle {o}"));
    }
    Some(lines)
}

/// The counts of each record kind, for the summary.
fn counts(records: &[TraceRecord]) -> String {
    let mut n = [0usize; 9];
    for r in records {
        let k = match r {
            TraceRecord::Message { .. } => 0,
            TraceRecord::Activate { .. } => 1,
            TraceRecord::Fire { .. } => 2,
            TraceRecord::Action { .. } => 3,
            TraceRecord::Timer { .. } => 4,
            TraceRecord::Rearm { .. } => 5,
            TraceRecord::Deactivate { .. } => 6,
            TraceRecord::State { .. } => 7,
            TraceRecord::Final { .. } => 8,
        };
        n[k] += 1;
    }
    format!(
        "messages {} activations {} firings {} actions {} timers {} rearms {} deactivations {} states {}",
        n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]
    )
}

#[test]
fn data_dump_mission_trace() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the mission trace was skipped");
        return;
    };
    let Some((mut world, mut logic)) = started(&root.join("EXE"), &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let records = run_session(&mut cx);
    let rust: Vec<String> = records.iter().map(TraceRecord::to_json).collect();
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "mission trace (Mission_01a, {TICKS} ticks): {} records; {}",
        rust.len(),
        counts(&records)
    );
    let Some(oracle_dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let mut jsonl = rust.join("\n");
    jsonl.push('\n');
    let _ = std::fs::write(oracle_dir.join("rust-mission.jsonl"), jsonl);
    let path = oracle_dir.join("mission_trace").join("mission.jsonl");
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(e) => panic!("{}: {e}", path.display()),
    };
    let oracle: Vec<String> = text
        .lines()
        .map(str::trim_end)
        .filter(|l| !l.is_empty())
        .map(str::to_owned)
        .collect();
    let difference = first_difference(&rust, &oracle);
    match &difference {
        None => {
            let _ = writeln!(summary, "equal to the oracle's {} records", oracle.len());
        }
        Some(lines) => {
            for line in lines {
                let _ = writeln!(summary, "{line}");
            }
        }
    }
    print!("{summary}");
    let _ = std::fs::write(oracle_dir.join("rust-mission-trace.txt"), &summary);
    assert!(
        difference.is_none(),
        "mission trace: differs from the oracle"
    );
}

#[test]
fn the_opening_runs_on_outdir() {
    let root = outdir();
    let Some((mut world, mut logic)) = started(&root, &root.join("data")) else {
        println!("notice: outdir/OutpostFrontier.ini is missing; the opening was not run");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let records = run_session(&mut cx);
    let id = |name: &[u8]| create_id(name);
    let fired = |trigger: u32| {
        records.iter().find_map(|r| match *r {
            TraceRecord::Fire {
                tick,
                trigger: t,
                event,
            } if t == trigger => Some((tick, event)),
            _ => None,
        })
    };
    // MissionStart fires the initial trigger through its probe.
    assert_eq!(
        fired(id(b"tr_initialize_init")),
        Some((0, engine::PROBE)),
        "{}",
        counts(&records)
    );
    // Mission_01a's constructor leaves the desired state 3: the first
    // message's tail applies it.
    assert!(records.contains(&TraceRecord::State {
        tick: 0,
        from: 2,
        to: 3
    }));
    // The camera trigger is activated by the first heartbeat's drain and
    // fires on SpaceEnter with its 19 actions.
    assert_eq!(fired(id(b"tr_fp7_cam")), Some((2, 0x36)));
    let actions = records
        .iter()
        .filter(
            |r| matches!(r, TraceRecord::Action { trigger, .. } if *trigger == id(b"tr_fp7_cam")),
        )
        .count();
    assert_eq!(actions, 19);
    // The 5 s timer counts the heartbeats after its activation at tick 2.
    let (tick, event) = fired(id(b"fp7_rendezvous_chatter_transport")).expect("the 5 s timer");
    assert_eq!(event, 0x20);
    assert!((150..=154).contains(&tick), "fired at tick {tick}");
    // Its Cnd_True follower fires through its probe in the same drain.
    assert_eq!(
        fired(id(b"fp7_attacker_spawn")),
        Some((tick, engine::PROBE))
    );
    // The scripted comms run the dialog chain.
    assert_eq!(fired(id(b"Samura_Ack_dlg")).map(|f| f.0), Some(200));
    assert_eq!(fired(id(b"Base_sends_escorts_trg")).map(|f| f.0), Some(260));
    // The base entry fires the cut scene's trigger.
    assert_eq!(fired(id(b"bse_play_2xe")).map(|f| f.0), Some(620));
    assert!(matches!(records.last(), Some(TraceRecord::Final { .. })));
}
