//! The Tier B router and machine trace of a random mission (work package
//! P3.7b, record 0008 point 10): the `DestroyMission` controller, its
//! runtime machines, their triggers and the message router of
//! `game_logic::content::random_mission::runtime`, against `Content.dll`'s
//! `OracleDumpRmSession` (`CppSource/Content/oracle_rm_session.cpp`,
//! session `CppSource/Oracle/rm_session.txt`).
//!
//! Both sides start a multiplayer server, register player 7 in
//! `he01_01_Base`'s system with no ship and no server slot, load the base's
//! rooms in the server's base database, and run the steps of
//! [`run_session`], each from `srand(SEED + step)`:
//!
//! 0. `BaseEnter` (GameSupport's case, then BaseSupport's);
//! 1. `RefreshMissionVendorOffers`;
//! 2. `MissionResponse` accepting the first vendor offer: BaseSupport makes
//!    the `DestroyMission` controller on the bus;
//! 3. player 7 joined to the mission (the oracle's `OracleJoinPlayer`, for
//!    the group a slotless player does not have);
//! 4. twelve ticks: the game time plus 1 s, `Heartbeat {1.0}` to the
//!    mission through the bus, the bus's timers fired with 1 s (the
//!    router's timers on SpaceSupport among them);
//! 5. `AbortMission` to the mission;
//! 6. the bus's heartbeat walk with 0: the queued mission torn down.
//!
//! The trace (`rm.jsonl`, the records of
//! `game_logic::content::random_mission::runtime::trace`) has the
//! router's registrations, routes and deliveries, each machine's enter and
//! exit events by state, the mission's end events, and an `end` record per
//! step with its `rand()` draws and player 7's flags, live mission and
//! random-mission counts. The comparison is exact, record by record, to
//! the first difference.
//!
//! - `data_dump_rm_session` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   against `rm_session/rm.jsonl`; summary `rust-rm-session.txt`, the
//!   port's trace `rust-rm.jsonl`.
//! - `the_rm_session_runs_on_outdir`: the session on `outdir/`, its records
//!   checked by hand; it returns early without the data.
//!
//! Nothing these tests write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::bases as base_db;
use game_logic::content::players::PlayerState;
use game_logic::content::random_mission::runtime::{self, trace};
use game_logic::content::{Instance, base_support, game_support};
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

/// `OracleRmSeed`: each step seeds `SEED + step`.
const SEED: u32 = 0x4f52_4143;
/// `OracleRmPlayerId`.
const PLAYER: u32 = 7;
/// `OracleRmBaseName`.
const BASE: &[u8] = b"he01_01_Base";
/// `OracleRmTicks`.
const TICKS: u32 = 12;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer server over a data root, started as the oracle starts it
/// (as `base_session.rs` does).
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

    fn message(&self, cx: &mut Cx<'_>, id: u32) {
        trace::push(
            cx,
            format!("{{\"rec\":\"message\",\"step\":{},\"msg\":{id}}}", self.step),
        );
    }

    /// The step's `end` record.
    fn end(&self, cx: &mut Cx<'_>) {
        let draws = draws(cx, SEED.wrapping_add(self.step));
        let counts = |m: &std::collections::BTreeMap<u8, i32>| {
            let parts: Vec<String> = m.iter().map(|(k, v)| format!("[{k},{v}]")).collect();
            parts.join(",")
        };
        let line = match cx.logic.content.state.players.get(&PLAYER) {
            Some(s) => format!(
                "{{\"rec\":\"end\",\"step\":{},\"draws\":{draws},\"flags\":{},\"live\":{},\"completed\":[{}],\"aborted\":[{}],\"failed\":[{}]}}",
                self.step,
                s.flags,
                s.live_mission,
                counts(&s.stats.random_missions_completed),
                counts(&s.stats.random_missions_aborted),
                counts(&s.stats.random_missions_failed)
            ),
            None => format!(
                "{{\"rec\":\"end\",\"step\":{},\"draws\":{draws},\"flags\":0,\"live\":0,\"completed\":[],\"aborted\":[],\"failed\":[]}}",
                self.step
            ),
        };
        trace::push(cx, line);
    }

    /// GameSupport's case for a base message, then BaseSupport's.
    fn base_send(&self, cx: &mut Cx<'_>, ids: (ControllerId, ControllerId), mut message: ControllerMessage<'_>) {
        self.message(cx, message.id());
        game_support::handle_message(cx, ids.0, &mut message);
        base_support::handle_message(cx, ids.1, &mut message);
    }
}

/// The ids of the started GameSupport and BaseSupport.
fn support_ids(cx: &Cx<'_>) -> (ControllerId, ControllerId) {
    let find = |want: fn(&Instance) -> bool| {
        cx.logic
            .content
            .instances
            .iter()
            .find(|(_, i)| want(i))
            .map_or(ControllerId(0), |(&id, _)| id)
    };
    (
        find(|i| matches!(i, Instance::GameSupport)),
        find(|i| matches!(i, Instance::BaseSupport)),
    )
}

/// The random mission's controller, if one was made.
fn mission_id(cx: &Cx<'_>) -> Option<ControllerId> {
    cx.logic
        .content
        .instances
        .iter()
        .find(|(_, i)| matches!(i, Instance::RandomMission(_)))
        .map(|(&id, _)| id)
}

/// The session's trace on a started server; `None` when no mission was
/// accepted.
fn run_session(cx: &mut Cx<'_>) -> (Vec<String>, Vec<&'static str>, bool) {
    let ids = support_ids(cx);
    let base = api::misc::base_id(cx, BASE).unwrap_or(0);
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
    s.begin(cx, "base_enter");
    s.base_send(
        cx,
        ids,
        ControllerMessage::BaseEnter {
            base,
            player: PLAYER,
        },
    );
    s.end(cx);

    s.begin(cx, "vendor_offers");
    base_support::refresh_mission_vendor_offers(cx, PLAYER);
    s.end(cx);

    let offer = cx
        .logic
        .content
        .state
        .base_visits
        .get(&PLAYER)
        .and_then(|v| v.vendor_offers.first())
        .map_or(0, |o| o.offer_id);
    s.begin(cx, "accept_vendor");
    s.base_send(
        cx,
        ids,
        ControllerMessage::MissionResponse {
            offer,
            kind: 2,
            accepted: true,
            player: PLAYER,
        },
    );
    s.end(cx);

    let Some(mission) = mission_id(cx) else {
        let lines = trace::stop(cx);
        cx.logic.content.state.players.remove(&PLAYER);
        return (lines, s.names, false);
    };
    s.begin(cx, "join");
    runtime::join_player(cx, mission, PLAYER);
    s.end(cx);

    for _ in 0..TICKS {
        s.begin(cx, "tick");
        cx.logic.content.state.game_time += 1.0;
        let mut heartbeat = ControllerMessage::Heartbeat { elapsed: 1.0 };
        s.message(cx, heartbeat.id());
        let _ = api::controller::send_message(cx, mission, &mut heartbeat);
        controller::fire_timers(cx, 1.0);
        s.end(cx);
    }

    s.begin(cx, "abort");
    let mut abort = ControllerMessage::AbortMission {
        player: PLAYER,
        mission: mission.0,
    };
    s.message(cx, abort.id());
    let _ = api::controller::send_message(cx, mission, &mut abort);
    s.end(cx);

    s.begin(cx, "teardown");
    controller::tick(cx, 0.0);
    controller::fire_timers(cx, 0.0);
    s.end(cx);

    let lines = trace::stop(cx);
    cx.logic.content.state.players.remove(&PLAYER);
    (lines, s.names, true)
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
            l[at..].split(|c: char| !c.is_ascii_digit()).next()?.parse::<usize>().ok()
        })
    };
    let s = step(rust.get(at)).or_else(|| step(oracle.get(at))).unwrap_or(0);
    let mut lines = vec![format!(
        "rm_session: first divergence at record {at} (step {s}, {}) of rust {} / oracle {}",
        names.get(s).copied().unwrap_or("?"),
        rust.len(),
        oracle.len()
    )];
    for i in at.saturating_sub(REPORT_WINDOW)..=at + REPORT_WINDOW {
        let mark = if i == at { ">" } else { " " };
        lines.push(format!("{mark} {i}: rust   {}", rust.get(i).map_or("-", String::as_str)));
        lines.push(format!("{mark} {i}: oracle {}", oracle.get(i).map_or("-", String::as_str)));
    }
    (lines, true)
}

/// The counts of each record kind, for the summary.
fn counts(lines: &[String]) -> String {
    let kinds = [
        "step",
        "message",
        "register",
        "unregister",
        "route",
        "deliver",
        "sm",
        "mission",
        "end",
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
fn data_dump_rm_session() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the random-mission session was skipped");
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
    let (lines, names, accepted) = run_session(&mut cx);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "random-mission session ({}, {} steps{}): {} records; {}",
        String::from_utf8_lossy(BASE),
        names.len(),
        if accepted { "" } else { ", no mission accepted" },
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
    let _ = std::fs::write(oracle_dir.join("rust-rm.jsonl"), jsonl);
    let path = oracle_dir.join("rm_session").join("rm.jsonl");
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
    let _ = std::fs::write(oracle_dir.join("rust-rm-session.txt"), &summary);
    assert!(!failed, "random-mission session: differs from the oracle");
}

fn step_lines<'a>(lines: &'a [String], names: &[&str], name: &str) -> Vec<&'a str> {
    let Some(step) = names.iter().position(|n| *n == name) else {
        return Vec::new();
    };
    let tag = format!("\"step\":{step},");
    lines
        .iter()
        .filter(|l| l.contains(&tag))
        .map(String::as_str)
        .collect()
}

#[test]
fn the_rm_session_runs_on_outdir() {
    let root = outdir();
    let Some((mut world, mut logic)) = started(&root, &root.join("data")) else {
        println!("notice: outdir/OutpostFrontier.ini is missing; the session was not run");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let (lines, names, accepted) = run_session(&mut cx);
    if let Some(out) = std::env::var_os("OUTPOST_RM_SESSION_OUT") {
        let _ = std::fs::write(out, lines.join("\n"));
    }
    assert!(accepted, "no vendor mission was accepted: {}", counts(&lines));
    let summary = counts(&lines);

    // The acceptance: the mission's group registers and enters state 0,
    // which routes its timeout 1.1 s later.
    let accept = step_lines(&lines, &names, "accept_vendor");
    assert!(
        accept.iter().any(|l| l.contains("\"kind\":\"mission\",\"event\":1,\"state\":0")),
        "{accept:#?}"
    );
    assert!(
        accept.iter().any(|l| l.contains("\"rec\":\"route\"") && l.contains("\"type\":\"Timeout\"")),
        "{accept:#?}"
    );

    // The timeout comes with the second tick: state 1 builds a vignette
    // machine per offer and starts the first after 0.01 s; the third tick
    // delivers the start, and the machine runs to state 8 or 10.
    let ticks: Vec<usize> = names
        .iter()
        .enumerate()
        .filter(|(_, n)| **n == "tick")
        .map(|(i, _)| i)
        .collect();
    let tick = |n: usize| -> Vec<&str> {
        let tag = format!("\"step\":{},", ticks[n]);
        lines.iter().filter(|l| l.contains(&tag)).map(String::as_str).collect()
    };
    let second = tick(1);
    assert!(
        second.iter().any(|l| l.contains("\"rec\":\"deliver\"") && l.contains("\"type\":\"Timeout\"") && l.contains("\"found\":1")),
        "{second:#?}"
    );
    assert!(second.iter().any(|l| l.contains("\"kind\":\"vignette\"")), "{second:#?}");
    let third = tick(2);
    assert!(
        third.iter().any(|l| l.contains("\"type\":\"StartMachine\"") && l.contains("\"found\":1")),
        "{third:#?}"
    );
    assert!(
        third.iter().any(|l| l.contains("\"type\":\"MachineStarted\"")),
        "{third:#?}"
    );
    assert!(
        third.iter().any(|l| l.contains("\"kind\":\"vignette\",\"event\":1,\"state\":8")
            || l.contains("\"kind\":\"vignette\",\"event\":1,\"state\":10")),
        "{third:#?}"
    );

    // The abort fails the mission (reason 1), ends it and counts it.
    let abort = step_lines(&lines, &names, "abort");
    assert!(abort.iter().any(|l| l.contains("\"event\":\"fail\",\"value\":1")), "{abort:#?}");
    assert!(abort.iter().any(|l| l.contains("\"event\":\"end\"")), "{abort:#?}");
    let end = abort.last().expect("the end record");
    assert!(end.contains("\"aborted\":[["), "{end}");

    // The teardown unregisters every machine and the mission's group.
    let teardown = step_lines(&lines, &names, "teardown");
    let registered = lines.iter().filter(|l| l.contains("\"rec\":\"register\"")).count();
    let unregistered = lines.iter().filter(|l| l.contains("\"rec\":\"unregister\"")).count();
    assert!(
        teardown.iter().filter(|l| l.contains("\"rec\":\"unregister\"")).count() >= 5,
        "{teardown:#?}"
    );
    assert!(unregistered <= registered, "{summary}");
    assert!(mission_id(&cx).is_none(), "the mission was torn down");
    assert!(
        cx.logic.content.state.router.as_ref().is_some_and(|r| r.pending().is_empty()),
        "{summary}"
    );
}
