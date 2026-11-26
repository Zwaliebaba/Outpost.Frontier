//! The Tier B story trace (work package P3.5d, record 0008 point 10 and
//! question 9): StoryLauncher, Story and the story missions' events and
//! overrides of `game_logic::content::{story, mission::story_missions}`,
//! against `Content.dll`'s `OracleDumpStorySession`
//! (`CppSource/Content/oracle_story_session.cpp` with the hooks of
//! `oracle_story_trace.h`, session `CppSource/Oracle/story_session.txt`).
//!
//! Both sides start a multiplayer server, register player 7 in
//! `he01_02_Base`'s system with no ship and no server slot, load the base's
//! rooms in the server's base database, and run the steps of
//! [`run_session`], each from `srand(SEED + step)`, every message through
//! the bus to one controller:
//!
//! 0. a StoryLauncher made on the bus (`BELOW_NORMAL`);
//! 1. `PlayerLogin {7}` to it: Story made;
//! 2. `PlayerLoad` of `newplayer.fl`'s `[StoryInfo]` to Story:
//!    `Mission_01a` made, loaded and started;
//! 3. `MissionResponse` accepting it: Story's event 1, chapter 2;
//! 4. its success (the desired state set and `ApplyDesiredMissionState`):
//!    Story's event 3, the mission destroyed, chapters 3 and 4,
//!    `Mission_01b` made and started;
//! 5. the bus's heartbeat walk with 0 (the teardown);
//! 6. `BaseEnter` of `he01_02_Base` (GameSupport's case, then to the
//!    mission): `Mission_01b`'s override joins, reports the acceptance and
//!    sets the attack clamp;
//! 7. its success: the mission ended, chapter 5 and its worth;
//! 8. the teardown;
//! 9. `PlayerWorthChanged {7}` to Story: the worth is below chapter 5's;
//! 10. `PlayerLoad` of a `[StoryInfo]` at chapter 21 with the worth 0;
//! 11. `PlayerWorthChanged {7}`: `Mission_07` made and started (chapter
//!     22);
//! 12. `PlayerShipArchSet {7}`: the standing with `gd_bh_grp`;
//! 13. `Mission_07`, Story and the launcher destroyed, then the teardown.
//!
//! The trace (`story.jsonl`, the records of
//! `game_logic::content::story::trace`) has each message to Story, each
//! `OnMissionEvent`, each create and destroy Story or StoryLauncher makes,
//! Story's state after each of its calls, a `send` record before each
//! message the session sends and an `end` record per step with its
//! `rand()` draws, player 7's mission number, live mission, the attack
//! clamp and his completed random missions. The comparison is exact,
//! record by record, to the first difference.
//!
//! - `data_dump_story_session` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   against `story_session/story.jsonl`; summary `rust-story-session.txt`,
//!   the port's trace `rust-story.jsonl`.
//! - `the_story_session_runs_on_outdir`: the session on `outdir/`, its
//!   records checked by hand; it returns early without the data.
//!
//! Nothing these tests write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::bases as base_db;
use game_logic::content::mission::engine::{self, MISSION_STATE_SUCCEED};
use game_logic::content::mission::runtime;
use game_logic::content::players::PlayerState;
use game_logic::content::story::trace;
use game_logic::content::{Instance, game_support};
use game_logic::events::ControllerMessage;
use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::ini::IniReader;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// `OracleStorySeed`: each step seeds `SEED + step`.
const SEED: u32 = 0x4f52_4143;
/// `OracleStoryPlayerId`.
const PLAYER: u32 = 7;
/// `OracleStoryBaseName`.
const BASE: &[u8] = b"he01_02_Base";
/// `OracleStoryNewPlayer`: `newplayer.fl`'s `[StoryInfo]`.
const NEW_PLAYER: &[u8] = b"[StoryInfo]\nMission = Mission_01a\nMissionNum = 1\ndelta_worth = -1\n";
/// `OracleStoryChapter21`.
const CHAPTER_21: &[u8] =
    b"[StoryInfo]\nship_bought = false\nMission = No_Mission\nMissionNum = 21\ndelta_worth = 0.000000\n";

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer server over a data root, started as the oracle starts it
/// (as `rm_session.rs` does).
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

/// The draws since `srand(seed)` (-1 beyond 0x10000).
fn draws(cx: &mut Cx<'_>, seed: u32) -> i64 {
    let now = cx.world.rng().clone();
    let mut probe = CrtRand::with_seed(seed);
    for n in 0..=0x10000 {
        if probe == now {
            return n;
        }
        probe.rand();
    }
    -1
}

/// The session's recorder over the port's trace.
struct Session {
    step: u32,
    names: Vec<&'static str>,
}

impl Session {
    fn begin(&mut self, cx: &mut Cx<'_>, name: &'static str) {
        self.step = self.names.len() as u32;
        self.names.push(name);
        trace::set_step(cx, self.step);
        trace::push(
            cx,
            format!(
                "{{\"rec\":\"step\",\"step\":{},\"name\":\"{name}\"}}",
                self.step
            ),
        );
        cx.world.rng().seed(SEED.wrapping_add(self.step));
    }

    /// The `send` record of a message the session sends.
    fn note_send(&self, cx: &mut Cx<'_>, id: u32) {
        trace::push(
            cx,
            format!("{{\"rec\":\"send\",\"step\":{},\"msg\":{id}}}", self.step),
        );
    }

    /// A message to one controller through the bus.
    fn send(&self, cx: &mut Cx<'_>, to: ControllerId, mut message: ControllerMessage<'_>) {
        self.note_send(cx, message.id());
        let _ = api::controller::send_message(cx, to, &mut message);
    }

    /// `PlayerLoad {7, a reader over text}` to Story.
    fn load(&self, cx: &mut Cx<'_>, story: ControllerId, text: &[u8]) {
        let mut reader = IniReader::new();
        reader.open_memory(text);
        self.send(
            cx,
            story,
            ControllerMessage::PlayerLoad {
                player: PLAYER,
                reader: &mut reader,
            },
        );
    }

    /// The step's `end` record.
    fn end(&self, cx: &mut Cx<'_>) {
        let draws = draws(cx, SEED.wrapping_add(self.step));
        let attack = cx.logic.content.state.clamps.attack_clamp;
        let line = match cx.logic.content.state.players.get(&PLAYER) {
            Some(s) => {
                let completed: Vec<String> = s
                    .stats
                    .random_missions_completed
                    .iter()
                    .map(|(k, v)| format!("[{k},{v}]"))
                    .collect();
                format!(
                    "{{\"rec\":\"end\",\"step\":{},\"draws\":{draws},\"num\":{},\"live\":{},\"attack\":{attack},\"completed\":[{}]}}",
                    self.step,
                    s.mission_number,
                    s.live_mission,
                    completed.join(",")
                )
            }
            None => format!(
                "{{\"rec\":\"end\",\"step\":{},\"draws\":{draws},\"num\":0,\"live\":0,\"attack\":{attack},\"completed\":[]}}",
                self.step
            ),
        };
        trace::push(cx, line);
    }
}

/// The started GameSupport's id.
fn game_support_id(cx: &Cx<'_>) -> ControllerId {
    cx.logic
        .content
        .instances
        .iter()
        .find(|(_, i)| matches!(i, Instance::GameSupport))
        .map_or(ControllerId(0), |(&id, _)| id)
}

/// The Story's id, once one is made (`OracleStoryNoteStory`).
fn story_id(cx: &Cx<'_>) -> ControllerId {
    cx.logic
        .content
        .instances
        .iter()
        .find(|(_, i)| matches!(i, Instance::Story(_)))
        .map_or(ControllerId(0), |(&id, _)| id)
}

/// The mission the factory made last: `GStoryMissionController`.
fn mission_id(cx: &Cx<'_>) -> ControllerId {
    cx.logic.content.state.story_mission
}

/// The mission's success, as an action's SUCCEED and the handler's tail
/// make it.
fn succeed(cx: &mut Cx<'_>, mission: ControllerId) {
    if let Some(s) = runtime::script_mut(cx, mission) {
        s.mission_state = MISSION_STATE_SUCCEED;
    }
    engine::apply_desired_state(cx, mission);
}

/// The bus's heartbeat walk and timers with 0.
fn teardown(cx: &mut Cx<'_>) {
    controller::tick(cx, 0.0);
    controller::fire_timers(cx, 0.0);
}

/// The session's trace on a started server, its step names and whether
/// every step ran (`None` when the base is unknown).
fn run_session(cx: &mut Cx<'_>) -> Option<(Vec<String>, Vec<&'static str>, bool)> {
    let support = game_support_id(cx);
    let base = api::misc::base_id(cx, BASE).unwrap_or(0);
    if !cx.logic.content.state.bases.contains_key(&base) {
        return None;
    }
    let system = api::misc::base_system(cx, base).ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = system.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
    if let Some(host) = cx.world.data_host() {
        base_db::load_base(cx, &host, base);
    }
    trace::start(cx);
    let mut s = Session {
        step: 0,
        names: Vec::new(),
    };
    let mut complete = false;

    s.begin(cx, "launcher");
    let launcher = api::controller::create(
        cx,
        b"Content.dll",
        b"StoryLauncher",
        &CreateParams::None,
        Priority::BelowNormal,
    );
    s.end(cx);

    s.begin(cx, "login");
    s.send(
        cx,
        launcher,
        ControllerMessage::PlayerLogin { player: PLAYER },
    );
    s.end(cx);
    let story = story_id(cx);

    let mut made = story.0 != 0;
    if made {
        s.begin(cx, "load");
        s.load(cx, story, NEW_PLAYER);
        s.end(cx);
        made = mission_id(cx).0 != 0;
    }
    if made {
        let mission = mission_id(cx);
        s.begin(cx, "accept");
        s.send(
            cx,
            mission,
            ControllerMessage::MissionResponse {
                offer: mission.0,
                kind: 0,
                accepted: true,
                player: PLAYER,
            },
        );
        s.end(cx);

        s.begin(cx, "succeed");
        succeed(cx, mission);
        s.end(cx);
        made = mission_id(cx) != mission;

        s.begin(cx, "teardown");
        teardown(cx);
        s.end(cx);
    }
    if made {
        let mission = mission_id(cx);
        s.begin(cx, "base_enter");
        let mut enter = ControllerMessage::BaseEnter {
            base,
            player: PLAYER,
        };
        s.note_send(cx, enter.id());
        game_support::handle_message(cx, support, &mut enter);
        let _ = api::controller::send_message(cx, mission, &mut enter);
        s.end(cx);

        s.begin(cx, "succeed");
        succeed(cx, mission);
        s.end(cx);

        s.begin(cx, "teardown");
        teardown(cx);
        s.end(cx);

        s.begin(cx, "worth");
        s.send(
            cx,
            story,
            ControllerMessage::PlayerWorthChanged { client: PLAYER },
        );
        s.end(cx);

        s.begin(cx, "load_chapter");
        s.load(cx, story, CHAPTER_21);
        s.end(cx);

        let previous = mission_id(cx);
        s.begin(cx, "worth");
        s.send(
            cx,
            story,
            ControllerMessage::PlayerWorthChanged { client: PLAYER },
        );
        s.end(cx);
        let made_07 = mission_id(cx) != previous;

        s.begin(cx, "ship_bought");
        s.send(
            cx,
            story,
            ControllerMessage::PlayerShipArchSet { client: PLAYER },
        );
        s.end(cx);

        s.begin(cx, "teardown");
        api::controller::destroy(cx, mission_id(cx));
        api::controller::destroy(cx, story);
        api::controller::destroy(cx, launcher);
        teardown(cx);
        s.end(cx);
        complete = made_07;
    }

    let lines = trace::stop(cx);
    cx.logic.content.state.players.remove(&PLAYER);
    Some((lines, s.names, complete))
}

/// Records shown on each side of a first difference.
const REPORT_WINDOW: usize = 3;

/// The comparison: the summary lines, and whether a difference was found.
fn compare(rust: &[String], oracle: &[String], names: &[&str]) -> (Vec<String>, bool) {
    let Some(at) = (0..rust.len().max(oracle.len())).find(|&i| rust.get(i) != oracle.get(i)) else {
        return (Vec::new(), false);
    };
    let step = |line: Option<&String>| {
        line.and_then(|l| {
            let at = l.find("\"step\":")? + 7;
            l[at..]
                .split(|c: char| !c.is_ascii_digit())
                .next()?
                .parse::<usize>()
                .ok()
        })
    };
    let s = step(rust.get(at))
        .or_else(|| step(oracle.get(at)))
        .unwrap_or(0);
    let mut lines = vec![format!(
        "story_session: first divergence at record {at} (step {s}, {}) of rust {} / oracle {}",
        names.get(s).copied().unwrap_or("?"),
        rust.len(),
        oracle.len()
    )];
    for i in at.saturating_sub(REPORT_WINDOW)..=at + REPORT_WINDOW {
        let mark = if i == at { ">" } else { " " };
        lines.push(format!(
            "{mark} {i}: rust   {}",
            rust.get(i).map_or("-", String::as_str)
        ));
        lines.push(format!(
            "{mark} {i}: oracle {}",
            oracle.get(i).map_or("-", String::as_str)
        ));
    }
    (lines, true)
}

/// The counts of each record kind, for the summary.
fn counts(lines: &[String]) -> String {
    let kinds = [
        "step", "send", "message", "event", "create", "destroy", "state", "end",
    ];
    let parts: Vec<String> = kinds
        .iter()
        .map(|k| {
            let tag = format!("\"rec\":\"{k}\"");
            format!("{k} {}", lines.iter().filter(|l| l.contains(&tag)).count())
        })
        .collect();
    parts.join(" ")
}

#[test]
fn data_dump_story_session() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the story session was skipped");
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
    let Some((lines, names, complete)) = run_session(&mut cx) else {
        panic!(
            "story session: {} is not a base",
            String::from_utf8_lossy(BASE)
        );
    };
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "story session ({}, {} steps{}): {} records; {}",
        String::from_utf8_lossy(BASE),
        names.len(),
        if complete { "" } else { ", stopped early" },
        lines.len(),
        counts(&lines)
    );
    let Some(oracle_dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let mut jsonl = lines.join("\n");
    jsonl.push('\n');
    let _ = std::fs::write(oracle_dir.join("rust-story.jsonl"), jsonl);
    let path = oracle_dir.join("story_session").join("story.jsonl");
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
    let (report, failed) = compare(&lines, &oracle, &names);
    if report.is_empty() {
        let _ = writeln!(summary, "equal to the oracle's {} records", oracle.len());
    }
    for line in &report {
        let _ = writeln!(summary, "{line}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle_dir.join("rust-story-session.txt"), &summary);
    assert!(!failed, "story session: differs from the oracle");
}

/// The records of step `step`.
fn step_lines(lines: &[String], step: usize) -> Vec<&str> {
    let tag = format!("\"step\":{step},");
    lines
        .iter()
        .filter(|l| l.contains(&tag))
        .map(String::as_str)
        .collect()
}

/// The records of kind `rec`.
fn of_kind<'a>(lines: &[&'a str], rec: &str) -> Vec<&'a str> {
    let tag = format!("\"rec\":\"{rec}\"");
    lines.iter().copied().filter(|l| l.contains(&tag)).collect()
}

#[test]
fn the_story_session_runs_on_outdir() {
    let root = outdir();
    let Some((mut world, mut logic)) = started(&root, &root.join("data")) else {
        println!("notice: outdir/OutpostFrontier.ini is missing; the session was not run");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (lines, names, complete) = run_session(&mut cx).expect("he01_02_Base");
    assert!(complete, "{}: {:#?}", counts(&lines), lines);
    assert_eq!(
        names,
        [
            "launcher",
            "login",
            "load",
            "accept",
            "succeed",
            "teardown",
            "base_enter",
            "succeed",
            "teardown",
            "worth",
            "load_chapter",
            "worth",
            "ship_bought",
            "teardown"
        ]
    );
    let step = |n: usize| step_lines(&lines, n);
    let end = |n: usize| *step(n).last().expect("an end record");

    // The launcher's heartbeat draws once; the login makes Story, whose
    // heartbeat draws once.
    assert!(end(0).contains("\"draws\":1,"), "{}", end(0));
    let login = step(1);
    let creates = of_kind(&login, "create");
    assert_eq!(creates.len(), 1, "{login:#?}");
    assert!(creates[0].contains("\"name\":\"Story\""), "{login:#?}");
    assert!(end(1).contains("\"draws\":1,"), "{}", end(1));

    // The new player's [StoryInfo] makes Mission_01a (NORMAL) and sets
    // chapter 1; the state keeps the worth -1.
    let load = step(2);
    assert!(
        of_kind(&load, "create")
            .iter()
            .any(|l| l.contains("\"name\":\"Mission_01a\"")),
        "{load:#?}"
    );
    let state = *of_kind(&load, "state").last().expect("a state");
    assert!(state.contains("\"num\":1,"), "{state}");
    assert!(state.contains("\"worth\":\"bf800000\""), "{state}");
    assert!(state.contains("\"mission\":\"Mission_01a\""), "{state}");
    assert!(end(2).contains("\"num\":1,"), "{}", end(2));

    // The acceptance: event 1 with chapter 2; the attack clamp set.
    let accept = step(3);
    let events = of_kind(&accept, "event");
    assert_eq!(events.len(), 1, "{accept:#?}");
    assert!(
        events[0].contains("\"kind\":1,\"chapter\":2,"),
        "{accept:#?}"
    );
    assert!(end(3).contains("\"num\":2,"), "{}", end(3));
    assert!(end(3).contains("\"attack\":1,"), "{}", end(3));

    // The success: event 3, the mission destroyed (Story hears its
    // ControllerDestroyed), chapters 3 and 4, Mission_01b made; the
    // completion of type 7 counted; EndMission clears the attack clamp.
    let succeed = step(4);
    let events = of_kind(&succeed, "event");
    assert!(
        events[0].contains("\"kind\":3,\"chapter\":2,"),
        "{succeed:#?}"
    );
    assert_eq!(of_kind(&succeed, "destroy").len(), 1, "{succeed:#?}");
    assert!(
        of_kind(&succeed, "message")
            .iter()
            .any(|l| l.contains("\"msg\":12}")),
        "{succeed:#?}"
    );
    assert!(
        of_kind(&succeed, "create")
            .iter()
            .any(|l| l.contains("\"name\":\"Mission_01b\"")),
        "{succeed:#?}"
    );
    assert!(end(4).contains("\"num\":4,"), "{}", end(4));
    assert!(end(4).contains("\"completed\":[[7,1]]"), "{}", end(4));
    assert!(end(4).contains("\"attack\":0,"), "{}", end(4));

    // Mission_01b's base enter: its override reports the acceptance
    // (chapter 4) and sets the attack clamp.
    let enter = step(6);
    let events = of_kind(&enter, "event");
    assert_eq!(events.len(), 1, "{enter:#?}");
    assert!(
        events[0].contains("\"kind\":1,\"chapter\":4,"),
        "{enter:#?}"
    );
    assert!(end(6).contains("\"attack\":1,"), "{}", end(6));

    // Mission_01b's success: chapter 5 and its worth, 3500 over a worth of
    // 0 (the slotless player has no cash).
    let succeed = step(7);
    let state = *of_kind(&succeed, "state").last().expect("a state");
    assert!(state.contains("\"num\":5,"), "{state}");
    assert!(
        state.contains(&format!("\"worth\":\"{:08x}\"", 3500.0f32.to_bits())),
        "{state}"
    );
    assert!(state.contains("\"mission\":\"No_Mission\""), "{state}");
    assert!(end(7).contains("\"completed\":[[7,2]]"), "{}", end(7));

    // The worth is below chapter 5's: nothing is made.
    assert!(of_kind(&step(9), "create").is_empty(), "{:#?}", step(9));

    // Chapter 21 with the worth 0: the next worth change starts
    // Mission_07 at chapter 22.
    let worth = step(11);
    assert!(
        of_kind(&worth, "create")
            .iter()
            .any(|l| l.contains("\"name\":\"Mission_07\"")),
        "{worth:#?}"
    );
    let state = *of_kind(&worth, "state").last().expect("a state");
    assert!(state.contains("\"num\":22,"), "{state}");
    assert!(state.contains("\"worth\":\"bf800000\""), "{state}");

    // A ship bought in chapter 22.
    let bought = step(12);
    let state = *of_kind(&bought, "state").last().expect("a state");
    assert!(state.contains("\"bought\":1,"), "{state}");

    // The teardown: Story hears the three ControllerDestroyed, and no
    // Story, launcher or mission is left.
    let last = step(13);
    assert_eq!(
        of_kind(&last, "message")
            .iter()
            .filter(|l| l.contains("\"msg\":12}"))
            .count(),
        3,
        "{last:#?}"
    );
    assert!(cx.logic.content.instances.values().all(|i| !matches!(
        i,
        Instance::Story(_) | Instance::StoryLauncher | Instance::Mission(_)
    )));
}
