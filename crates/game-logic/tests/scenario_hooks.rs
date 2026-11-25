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
//! oracle's); the other scenarios do not.
//!
//! With `OUTPOST_ORACLE_DIR` set, each run is judged against the oracle's
//! `tick.jsonl` as `game-sim/tests/scenario_replay.rs` judges the run
//! without a server (`game_sim::scenario::judge`): bit for bit up to the
//! first contact or munition impact, the guided munitions' launches and
//! ends, the gross bound, and the outcome after it reported. The fights
//! take the pilots' personalities and the ships' attitudes from the
//! oracle's `ai.jsonl`. The summary goes to `rust-scenario-hooks.txt` in
//! the oracle folder; the test fails only on what `scenario_replay` fails
//! on. Without the variable only the [`SMOKE`] scenarios run, without a
//! comparison: they need no oracle input, their collision damage goes
//! through the server's damage hooks, and they are quick in a debug build
//! (where `fire_at_target`'s bullets alone take minutes).

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world::Hooks;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, DamageState, ServerHooks, ServerRequest};
use game_sim::object::{ObjHandle, World};
use game_sim::scenario::{
    self, dist, judge, needs_ai, oracle_ai_inputs, oracle_run, parse, report_guided,
    scenario_files, world_config,
};
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::Priority;

/// The scenarios run without the oracle: two contacts, an AI ship, the
/// two trade lanes (the instant one and the dock behaviour's, whose
/// request SpaceSupport answers) and the dock at a station, whose request
/// SpaceSupport's dock queues answer.
const SMOKE: [&str; 6] = [
    "ram_ship",
    "ram_asteroid",
    "turn_to_heading",
    "trade_lane",
    "dock_trade_lane",
    "dock_at_station",
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

/// The server's hooks with the physics of every sub-step on, as the
/// oracle's harness forces it for the scenario's system: every other
/// method is the server's.
struct ScenarioHooks<'l> {
    server: Hooks<'l>,
    /// The AI's requests go to the server (a scenario marked `# rust:
    /// server`); the other scenarios run without, as they were judged
    /// before.
    requests: bool,
}

impl ServerHooks for ScenarioHooks<'_> {
    fn object_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) -> bool {
        self.server.object_update(world, object, dt)
    }

    fn object_update_flags(&mut self, world: &World, object: ObjHandle) -> u32 {
        self.server.object_update_flags(world, object)
    }

    fn object_after_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        self.server.object_after_update(world, object, dt);
    }

    fn damage_state(
        &mut self,
        world: &World,
        object: ObjHandle,
        inflictor_owner: u32,
    ) -> DamageState {
        self.server.damage_state(world, object, inflictor_owner)
    }

    fn before_damage_list(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut DamageList,
    ) -> bool {
        self.server.before_damage_list(world, object, list)
    }

    fn after_damage_list(&mut self, world: &mut World, object: ObjHandle, list: &mut DamageList) {
        self.server.after_damage_list(world, object, list);
    }

    fn damage_list_done(&mut self, world: &mut World, object: ObjHandle) {
        self.server.damage_list_done(world, object);
    }

    fn destroy(&mut self, world: &mut World, object: ObjHandle, kind: i32, killer: u32) -> bool {
        self.server.destroy(world, object, kind, killer)
    }

    fn after_reap(&mut self, world: &mut World, system: u32) {
        self.server.after_reap(world, system);
    }

    fn server_requests(&mut self, world: &mut World, requests: Vec<ServerRequest>) {
        if self.requests {
            self.server.server_requests(world, requests);
        }
    }

    fn steps_physics(&mut self, _world: &World, _system: u32) -> bool {
        true
    }
}

/// The oracle's start-up (`Oracle.exe sim`, `WriteServerIni` and
/// `Server_LoadWorldState`): Content's GameSupport, SpaceSupport and
/// BaseSupport, then `LoadGame` over `InitialWorld.ini`, which builds the
/// system records. The constructors' heartbeat draws come before the
/// driver's seeding point, as the oracle's do.
fn content_start_up(cx: &mut Cx<'_>) {
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = neuron_server::controller::create(
            cx,
            b"Content.dll",
            name,
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{}", String::from_utf8_lossy(name));
    }
    let text = fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    game_logic::selection::load_world_state(cx, &mut reader);
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
            content_start_up(&mut cx);
        }
        let mut hooks = ScenarioHooks {
            server: Hooks { logic: &mut logic },
            requests: s.rust_server.is_some(),
        };
        let ours = scenario::run(world, &s, &ai, &mut hooks);
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
            continue;
        };
        let Some(theirs) = oracle_run(dir, &s.name) else {
            let _ = writeln!(summary, "{}: no oracle tick.jsonl", s.name);
            continue;
        };
        judge(&mut summary, &mut gross, dir, &s, &ai, &ours, &theirs);
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
