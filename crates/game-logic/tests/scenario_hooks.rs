//! The oracle scenarios through the server's hooks (decision O45): each
//! scenario of `CppSource/Oracle/scenarios` runs with game-sim's driver
//! (`game_sim::scenario::run`) on the original data in `outdir/`, its
//! system stepped through `game_logic::world::Hooks` over a multiplayer
//! [`Logic`] instead of `NoServerHooks`. The oracle's harness forces the
//! physics of a scenario's system on (`GOracleSimHooks.StepSystemId`),
//! where the server steps it only while the system has players, so the
//! hooks are wrapped ([`ScenarioHooks`]) to step it every sub-step. Every
//! scenario runs on the oracle's start-up (`Oracle.exe sim`'s reduced
//! `[Initial MP DLLs]`: Content's GameSupport, SpaceSupport and
//! BaseSupport, then `Server_LoadWorldState` over `InitialWorld.ini`), so
//! SpaceSupport runs on both sides (P3.6a). A scenario marked `# rust:
//! server` sends the AI's requests to the server
//! (`ServerHooks::server_requests`): the trade lanes (SpaceSupport answers
//! `dock_trade_lane`'s lane request, and the server's
//! `notify_stop_tradelane` ends both lanes' behaviours) and
//! `dock_at_station` (SpaceSupport's dock queues answer the dock request:
//! the fighter is no Content ship, so the answer is 5 with port -1, as the
//! oracle's); the other scenarios do not. A scenario's `content` ships
//! (P3.6c) are Content's NPC ships, made through the server's record
//! (`FSystemInfo::CreateNpcShip`), so their managers report to Content
//! and ask it for their flee destination (`content_follow`,
//! `content_guide`, `content_flee`). `content_flee_comms` (P3.6b) is
//! `content_flee` with Content's comm system on, on both sides; each
//! scenario's line says which comms the Rust side admitted (reported).
//! `populated_he01` (P3.6d) loads its system as the server does, makes
//! the ship player 1's and starts SpacePop (`OracleSimPopulate`); the
//! ships the population spawns are named `npc<k>` by the order they are
//! first found in the system's record, as the oracle's `tick.jsonl` names
//! them, and judged apart from the scenario's objects: their count and
//! each one's archetype against the oracle's `npc` records, their states
//! through the first-divergence report (reported).
//!
//! With `OUTPOST_ORACLE_DIR` set, each run is judged against the oracle's
//! `tick.jsonl` as `game-sim/tests/scenario_replay.rs` judges the run
//! without a server (`game_sim::scenario::judge`): bit for bit up to the
//! first contact or munition impact, the guided munitions' launches and
//! ends, the gross bound, and the outcome after it reported. The fights
//! take the pilots' personalities from the oracle's `ai.jsonl`; each
//! ship's `faction` gets a vibe of that reputation group through the
//! server's `pub::Reputation` (as the oracle's harness makes it), so the
//! world answers the attitudes from the initial world's feelings, and
//! Content's reputation changes in the fight reach them (P3.11b). Every object's line says whether it is bit-exact
//! over the run and, if not, gives its first divergence bit for bit
//! (`game_sim::scenario::report_first_divergence`, reported, not judged).
//! The summary goes to `rust-scenario-hooks.txt` in the oracle folder; the
//! test fails only on what `scenario_replay` fails on. Without the
//! variable only the [`SMOKE`] scenarios run, without the oracle's
//! comparison: they need no oracle input, their collision damage goes
//! through the server's damage hooks, and they are quick in a debug build
//! (where `fire_at_target`'s bullets alone take minutes). Each is then set
//! beside its run without a server, whose first divergence shows where
//! the server's part first changes the run (reported).

mod scenario_server;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, NoServerHooks};
use game_sim::scenario::records::{Json, find_files, read_lines};
use game_sim::scenario::{
    self, Run, Scenario, State, dist, judge, needs_ai, oracle_ai_inputs, oracle_run, parse,
    report_first_divergence, report_first_divergence_from, report_guided, scenario_files,
    world_config,
};
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use scenario_server::{ScenarioHooks, content_start_up, population_ship};

/// The scenarios run without the oracle: two contacts, an AI ship, the
/// two trade lanes (the instant one and the dock behaviour's, whose
/// request SpaceSupport answers), the dock at a station, whose request
/// SpaceSupport's dock queues answer, the launch out of a docking ring
/// (`pub::SpaceObj::Launch`, P3.11), Content's NPC ships following and
/// guiding (P3.6c), and free flight in he01 with SpacePop populating
/// around the ship (P3.6d).
const SMOKE: [&str; 10] = [
    "ram_ship",
    "ram_asteroid",
    "turn_to_heading",
    "trade_lane",
    "dock_trade_lane",
    "dock_at_station",
    "ring_launch",
    "content_follow",
    "content_guide",
    "populated_he01",
];

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

/// The oracle's `npc` records (`session.jsonl` beside the first
/// `tick.jsonl`, P3.6d): the ships the population spawned, by name, with
/// their ids and archetypes, in spawn order.
fn oracle_population(dir: &Path, scenario: &str) -> Vec<(String, u32, u32)> {
    let Some(tick) = find_files(&dir.join(scenario), "tick.jsonl")
        .into_iter()
        .next()
    else {
        return Vec::new();
    };
    let session = tick.with_file_name("session.jsonl");
    read_lines(&session)
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("npc"))
        .filter_map(|r| {
            Some((
                r.get("obj")?.as_str()?.to_owned(),
                r.get("id").and_then(Json::as_u32).unwrap_or(0),
                r.get("arch").and_then(Json::as_u32).unwrap_or(0),
            ))
        })
        .collect()
}

/// The population's ships (`npc<k>`) taken out of a run's states.
fn take_population(run: &mut Run) -> BTreeMap<String, Vec<State>> {
    let names: Vec<String> = run
        .states
        .keys()
        .filter(|n| is_population_name(n))
        .cloned()
        .collect();
    names
        .into_iter()
        .filter_map(|n| run.states.remove(&n).map(|v| (n, v)))
        .collect()
}

/// A ship of the population: its name, id and archetype.
type PopulationShip = (String, u32, u32);

/// Each object's states by name.
type States = BTreeMap<String, Vec<State>>;

/// `npc<k>`: a ship the population spawned.
fn is_population_name(name: &str) -> bool {
    name.strip_prefix("npc")
        .is_some_and(|k| !k.is_empty() && k.bytes().all(|b| b.is_ascii_digit()))
}

/// The populated scenario's NPC ships against the oracle's (P3.6d): their
/// count and each one's archetype by spawn order are judged; their states
/// go through the first-divergence report (reported, not judged), under
/// the scenario's name followed by `population`.
fn judge_population(
    summary: &mut String,
    gross: &mut Vec<String>,
    s: &Scenario,
    ours: (&[PopulationShip], States),
    theirs: (&[PopulationShip], States),
) {
    let (our_ships, our_states) = ours;
    let (their_ships, their_states) = theirs;
    let _ = writeln!(
        summary,
        "{}: population: rust spawned {} ships, oracle {}",
        s.name,
        our_ships.len(),
        their_ships.len()
    );
    if our_ships.len() != their_ships.len() {
        gross.push(format!(
            "{}: population: rust spawned {} ships, oracle {}",
            s.name,
            our_ships.len(),
            their_ships.len()
        ));
    }
    let mut archetypes = Vec::new();
    for (k, (ours, theirs)) in our_ships.iter().zip(their_ships).enumerate() {
        let first = |states: &BTreeMap<String, Vec<State>>, name: &str| {
            states
                .get(name)
                .and_then(|v| v.iter().position(|st| st.alive))
                .map_or_else(|| "-".to_owned(), |t| t.to_string())
        };
        let _ = writeln!(
            summary,
            "{}: population: {}: rust id {} arch {:#010x} from tick {} | oracle id {} arch {:#010x} from tick {}",
            s.name,
            ours.0,
            ours.1,
            ours.2,
            first(&our_states, &ours.0),
            theirs.1,
            theirs.2,
            first(&their_states, &theirs.0)
        );
        if ours.0 != theirs.0 || ours.2 != theirs.2 {
            archetypes.push(k + 1);
        }
    }
    if !archetypes.is_empty() {
        gross.push(format!(
            "{}: population: the archetype differs for ship(s) {archetypes:?} by spawn order",
            s.name
        ));
    }
    let mut named = s.clone();
    named.name = format!("{} population", s.name);
    let ours = Run {
        states: our_states,
        ..Run::default()
    };
    let theirs = Run {
        states: their_states,
        ..Run::default()
    };
    report_first_divergence(summary, &named, &ours, &theirs, None);
}

#[test]
fn scenarios_through_the_server_hooks() {
    let Some(data) = game_data() else {
        println!("outdir/ is missing: no scenario was run through the server's hooks");
        return;
    };
    let oracle = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from);
    if oracle.is_none() {
        println!(
            "OUTPOST_ORACLE_DIR is not set: only {} run through the server's hooks, without the oracle's comparison",
            SMOKE.join(", ")
        );
    }
    let mut summary = String::new();
    let mut gross = Vec::new();
    for path in scenario_files() {
        let Some(s) = parse(&path) else {
            let _ = writeln!(summary, "{}: does not parse", path.display());
            continue;
        };
        if let Some(why) = needs_ai(&s) {
            let _ = writeln!(summary, "{}: skipped, {why}", s.name);
            continue;
        }
        if oracle.is_none() && !SMOKE.contains(&s.name.as_str()) {
            continue;
        }

        let ai = oracle
            .as_ref()
            .map(|d| oracle_ai_inputs(d, &s.name))
            .unwrap_or_default();
        let mut world = World::new(data.clone(), world_config(&s))
            .with_assets(Assets::new(outdir().join("data")));
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
        {
            let mut cx = Cx {
                world: &mut world,
                logic: &mut logic,
            };
            content_start_up(&mut cx, &outdir());
            // `comms off`: the GCS clamp, as the oracle's
            // `OracleSimSetGcsClamp` sets it, so Content's comm system
            // admits nothing and draws no roll (P3.6b).
            if s.comms_off {
                cx.logic.content.state.clamps.set_gcs_clamp_enabled(1);
            }
        }
        let mut hooks =
            ScenarioHooks::new(&mut logic, s.rust_server.is_some(), population_ship(&path));
        let mut ours = scenario::run_with(world, &s, &ai, &mut hooks);
        let our_ships = std::mem::take(&mut hooks.population_ships);
        drop(hooks);
        // The population's ships (P3.6d) apart from the scenario's objects.
        let our_population = take_population(&mut ours);
        for (name, id, arch) in &our_ships {
            let first = our_population
                .get(name)
                .and_then(|v| v.iter().position(|st| st.alive));
            let _ = writeln!(
                summary,
                "{}: population: {name}: id {id} arch {arch:#010x} spawned at tick {}",
                s.name,
                first.map_or_else(|| "-".to_owned(), |t| t.to_string())
            );
        }
        // The comm system's activity (P3.6b; reported, not judged): the
        // comms it admitted, newest ten per category.
        let recent = &logic.content.state.gcs.recent;
        if recent.values().any(|r| !r.is_empty()) {
            let per: Vec<String> = recent
                .iter()
                .filter(|(_, r)| !r.is_empty())
                .map(|(c, r)| format!("{c:#x}: {}", r.len()))
                .collect();
            let _ = writeln!(
                summary,
                "{}: comm system: comms admitted by category (at most 10 kept) {}",
                s.name,
                per.join(", ")
            );
        }
        if let Some((b, tick)) = ours.unported {
            let _ = writeln!(
                summary,
                "{}: skipped, an AI ship runs behaviour {b} from tick {tick}, which is not ported",
                s.name
            );
            if let Some(dir) = &oracle {
                report_guided(&mut summary, &mut gross, dir, &s.name, &ours, u32::MAX);
            }
            continue;
        }
        for (name, states) in &ours.states {
            if let Some(last) = states.last() {
                let _ = writeln!(
                    summary,
                    "{}: {name} after {} ticks: alive {} pos ({:.3}, {:.3}, {:.3}) speed {:.3} hp {}",
                    s.name,
                    states.len() - 1,
                    last.alive,
                    last.pos.x,
                    last.pos.y,
                    last.pos.z,
                    dist(last.vel, Vector::ZERO),
                    last.hp
                );
            }
        }
        let Some(dir) = &oracle else {
            // Without the oracle, the run beside the same scenario's run
            // without a server (game-sim's `NoServerHooks`, no Content):
            // the first divergence is where the server's part first
            // changes the run (reported, not judged).
            let mut world = World::new(data.clone(), world_config(&s))
                .with_assets(Assets::new(outdir().join("data")));
            let text = fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
            scenario::load_initial_feelings(&mut world, &text);
            let alone = scenario::run(world, &s, &ai, &mut NoServerHooks);
            let _ = writeln!(
                summary,
                "{}: through the server's hooks against the run without a server (no oracle; reported):",
                s.name
            );
            report_first_divergence_from(
                &mut summary,
                &s,
                &ours,
                &alone,
                None,
                ("hooks", "no-server"),
            );
            continue;
        };
        let Some(mut theirs) = oracle_run(dir, &s.name) else {
            let _ = writeln!(summary, "{}: no oracle tick.jsonl", s.name);
            continue;
        };
        let their_population = take_population(&mut theirs);
        judge(&mut summary, &mut gross, dir, &s, &ai, &ours, &theirs);
        if population_ship(&path).is_some() {
            let their_ships = oracle_population(dir, &s.name);
            judge_population(
                &mut summary,
                &mut gross,
                &s,
                (&our_ships, our_population),
                (&their_ships, their_population),
            );
        }
    }
    let _ = writeln!(
        summary,
        "gross divergences: {}",
        if gross.is_empty() {
            "none".to_owned()
        } else {
            gross.join("; ")
        }
    );
    print!("{summary}");
    if let Some(dir) = &oracle {
        let _ = fs::write(dir.join("rust-scenario-hooks.txt"), &summary);
    }
    assert!(gross.is_empty(), "gross divergence: {gross:?}");
}
