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

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::content::npc_ships::{self, NpcShipParams};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world::Hooks;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, DamageState, NoServerHooks, ServerHooks, ServerRequest};
use game_sim::object::{ObjHandle, World};
use game_sim::scenario::{
    self, ScenarioServer, dist, judge, needs_ai, oracle_ai_inputs, oracle_run, parse,
    report_first_divergence_from, report_guided, scenario_files, world_config,
};
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::Priority;

/// The scenarios run without the oracle: two contacts, an AI ship, the
/// two trade lanes (the instant one and the dock behaviour's, whose
/// request SpaceSupport answers), the dock at a station, whose request
/// SpaceSupport's dock queues answer, the launch out of a docking ring
/// (`pub::SpaceObj::Launch`, P3.11), and Content's NPC ships following
/// and guiding (P3.6c).
const SMOKE: [&str; 9] = [
    "ram_ship",
    "ram_asteroid",
    "turn_to_heading",
    "trade_lane",
    "dock_trade_lane",
    "dock_at_station",
    "ring_launch",
    "content_follow",
    "content_guide",
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
    /// The damage lists and destructions in the order the world makes
    /// them, with the frame length to number the ticks
    /// ([`guided_impact_order`]); `None` keeps none.
    trace: Option<(f64, Vec<Note>)>,
}

/// A damage list or a destruction the world made (tick numbered from 1).
#[derive(Debug)]
struct Note {
    tick: u64,
    /// The object's id and kind.
    object: (u32, Option<game_sim::object::ObjectKind>),
    /// A damage list's cause and entries (sub-object, hit points after);
    /// `None` for a destruction.
    damage: Option<(i32, Vec<(u16, f32)>)>,
}

impl ScenarioHooks<'_> {
    fn note(&mut self, world: &World, object: ObjHandle, list: Option<&DamageList>) {
        if let Some((frame, notes)) = &mut self.trace {
            let tick = (world.time() / *frame).round() as u64 + 1;
            let o = world.get(object);
            notes.push(Note {
                tick,
                object: (o.map_or(0, |o| o.simple.id), o.map(|o| o.kind)),
                damage: list.map(|l| {
                    (
                        l.cause,
                        l.entries
                            .iter()
                            .map(|e| (e.sub_obj_id, e.hit_pts))
                            .collect(),
                    )
                }),
            });
        }
    }
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
        self.note(world, object, Some(list));
        self.server.before_damage_list(world, object, list)
    }

    fn damage_entry(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut DamageList,
        index: usize,
    ) {
        self.server.damage_entry(world, object, list, index);
    }

    fn after_damage_list(&mut self, world: &mut World, object: ObjHandle, list: &mut DamageList) {
        self.server.after_damage_list(world, object, list);
    }

    fn damage_list_done(&mut self, world: &mut World, object: ObjHandle) {
        self.server.damage_list_done(world, object);
    }

    fn destroy(&mut self, world: &mut World, object: ObjHandle, kind: i32, killer: u32) -> bool {
        self.note(world, object, None);
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

    fn content_answer(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
    ) -> Option<game_sim::ai::content::FleeAnswer> {
        self.server.content_answer(world, ship, handle)
    }

    fn directive_complete(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
        result: (i32, i32),
    ) {
        self.server.directive_complete(world, ship, handle, result);
    }

    /// The end of a `LaunchAction` (`ring_launch`): `Ship::slot_70` lifts
    /// the launch invincibility with `LaunchComplete`.
    fn launch_complete(&mut self, world: &mut World, object: ObjHandle, base: u32) {
        self.server.launch_complete(world, object, base);
    }
}

impl ScenarioServer for ScenarioHooks<'_> {
    fn hooks(&mut self) -> &mut dyn ServerHooks {
        self
    }

    /// A `faction`'s vibe as the oracle's harness makes it
    /// (`AllocFactionVibe`): `pub::Reputation::GetReputationGroup`, then
    /// `Alloc` with empty names (the server's vibe id) and
    /// `SetAffiliation`.
    fn faction_vibe(&mut self, world: &mut World, faction: &str) -> Option<i32> {
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        faction_vibe(&mut cx, faction)
    }

    /// `spaceobj_launch` (`ring_launch`): `pub::SpaceObj::Launch(ship, dock,
    /// type)`, the server's `StarSystem::launch`.
    fn launch(&mut self, world: &mut World, ship: ObjHandle, dock: ObjHandle, kind: i32) -> bool {
        let (Some(ship), Some(dock)) = (
            world.get(ship).map(|o| o.simple.id),
            world.get(dock).map(|o| o.simple.id),
        ) else {
            return false;
        };
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        let _ = game_logic::api::space_obj::launch(&mut cx, ship, dock, kind);
        true
    }

    /// A `content` ship (P3.6c): Content's NPC ship in the system's record
    /// (`FSystemInfo::CreateNpcShip`), as the oracle's
    /// `OracleSimCreateNpcShip` makes it: the archetype, loadout, place,
    /// hit points, state graph (`LEADER`) and personality (the default, or
    /// the pilot's), owned by SpaceSupport, with a reputation handle of its
    /// `faction`'s vibe (`FOracleNpcReputation`, which frees nothing) and
    /// the `flee` position.
    fn spawn_content_ship(
        &mut self,
        world: &mut World,
        system: u32,
        object: &game_sim::scenario::ObjDesc,
        personality: Option<game_sim::ai::personality::Personality>,
        vibe: i32,
    ) -> Option<Option<ObjHandle>> {
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        let state_graph = object.ai.as_ref().map_or(-1, |g| {
            cx.world
                .data()
                .state_graphs
                .find(g.as_bytes(), game_sim::ai::state_graph::GraphType::Leader)
        });
        let reputation = cx
            .logic
            .content
            .state
            .rep_handles
            .create(vibe, game_logic::content::rep_handles::RepKind::Plain);
        let params = NpcShipParams {
            reputation: Some(reputation),
            archetype: neuron_core::id::create_id(object.archetype.as_bytes()),
            loadout: if object.loadout.is_empty() {
                0
            } else {
                neuron_core::id::create_id(object.loadout.as_bytes())
            },
            position: object.pos,
            orientation: object.orient,
            hit_points: object.hp as i32,
            state_graph,
            personality: personality.unwrap_or_default(),
            controller: cx.logic.content.state.space_support.0,
            ..NpcShipParams::default()
        };
        let (_, key) = npc_ships::create_in_system(&mut cx, system, &params, 0);
        npc_ships::release_params(&mut cx, params);
        let key = key?;
        if let Some((point, distance)) = object.flee {
            npc_ships::set_flee_position(&mut cx, key, point, distance);
        }
        let id = npc_ships::space_obj(&cx, key);
        Some(cx.world.find(id))
    }
}

/// `AllocFactionVibe` (`oracle_sim.cpp:2079-2098`) through the server's
/// `pub::Reputation`: `None` for an unknown group or a failed call.
fn faction_vibe(cx: &mut Cx<'_>, faction: &str) -> Option<i32> {
    let group = game_logic::api::reputation::reputation_group(cx, faction.as_bytes()).ok()?;
    let name = game_sim::protocol::fmtstr::FmtStr::default();
    let vibe = game_logic::api::reputation::alloc(cx, &name, &name).ok()?;
    game_logic::api::reputation::set_affiliation(cx, vibe, group).ok()?;
    Some(vibe)
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
            // `comms off`: the GCS clamp, as the oracle's
            // `OracleSimSetGcsClamp` sets it, so Content's comm system
            // admits nothing and draws no roll (P3.6b).
            if s.comms_off {
                cx.logic.content.state.clamps.set_gcs_clamp_enabled(1);
            }
        }
        let mut hooks = ScenarioHooks {
            server: Hooks { logic: &mut logic },
            requests: s.rust_server.is_some(),
            trace: None,
        };
        let ours = scenario::run_with(world, &s, &ai, &mut hooks);
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

/// A guided munition's impact on a ship (P3.11c), without the oracle:
/// `dogfight` runs through the server's hooks up to the police's first
/// missile's contact with the rogue (tick 236), with each pilot's
/// personality from Content's pilot table (`OracleSimPilotPersonality`, as
/// the oracle's harness gets it). As in the reference, the missile's direct
/// hit (`Guided::on_collision`, `StarSystem::munition_collision`, cause 3)
/// comes first (D1085), then the rogue's collision damage destroys the
/// missile and `Guided::destroy` explodes it (cause 5), and the blast takes
/// its full band from the rogue's shield, whose sphere is the ship's
/// (`CEShield::Connect`): `(747 * 0.5 + 0) * (3 * 1/3)`, times the shield's
/// explosion resistance of 1. The damage lists and destructions of the
/// impact's tick are printed in order.
#[test]
fn guided_impact_order() {
    let Some(data) = game_data() else {
        println!("outdir/ is missing: no fight was run");
        return;
    };
    let Some(mut s) = scenario_files()
        .into_iter()
        .filter_map(|p| parse(&p))
        .find(|s| s.name == "dogfight")
    else {
        panic!("no scenario dogfight");
    };
    s.ticks = s.ticks.min(240);
    let mut world =
        World::new(data.clone(), world_config(&s)).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    {
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        content_start_up(&mut cx);
        if s.comms_off {
            cx.logic.content.state.clamps.set_gcs_clamp_enabled(1);
        }
    }
    let mut ai = game_sim::scenario::AiInputs::default();
    for o in &s.objects {
        if let Some(pilot) = &o.pilot {
            let mut p = game_sim::ai::personality::Personality::default();
            assert!(
                logic
                    .content
                    .tables
                    .pilots
                    .personality(neuron_core::id::create_id(pilot.as_bytes()), &mut p)
            );
            ai.personalities.insert(o.name.clone(), p);
        }
    }
    let mut hooks = ScenarioHooks {
        server: Hooks { logic: &mut logic },
        requests: s.rust_server.is_some(),
        trace: Some((s.frame, Vec::new())),
    };
    let _ = scenario::run_with(world, &s, &ai, &mut hooks);
    let (_, notes) = hooks.trace.take().unwrap_or_default();
    let guided = Some(game_sim::object::ObjectKind::Guided);
    let is_kill = |n: &Note| n.object.1 == guided && n.damage.is_none();
    let Some(at) = notes.iter().position(is_kill) else {
        panic!("the missile was not destroyed");
    };
    let tick = notes[at].tick;
    let impact: Vec<&Note> = notes.iter().filter(|n| n.tick == tick).collect();
    for n in &impact {
        println!("dogfight: {n:?}");
    }
    assert_eq!(tick, 236, "the missile's impact");
    let cause = |n: &Note| n.damage.as_ref().map(|d| d.0);
    let hit = impact.iter().position(|n| cause(n) == Some(3));
    let destroyed = impact.iter().position(|n| is_kill(n));
    let blast = impact.iter().position(|n| cause(n) == Some(5));
    assert!(
        hit.is_some() && hit < destroyed && destroyed < blast,
        "the direct hit, the missile's destruction, then its explosion: {impact:?}"
    );
    let blast = impact[blast.unwrap_or_default()];
    // The rogue's shield before the blast (its last entry written before
    // the impact's tick, which the tick's regeneration raises by a few
    // hundredths) and after it: the full band, not two thirds (249).
    let after = blast.damage.as_ref().and_then(|d| d.1.first().copied());
    let before = notes[..at]
        .iter()
        .rev()
        .filter(|n| n.object.0 == blast.object.0 && n.tick < tick)
        .find_map(|n| n.damage.as_ref().and_then(|d| d.1.first().copied()));
    let (Some((sub, after)), Some((sub_before, before))) = (after, before) else {
        panic!("no shield entries: {impact:?}");
    };
    assert_eq!(sub, sub_before, "the shield");
    assert!(
        (f64::from(before) - f64::from(after) - 373.5).abs() < 0.1,
        "the blast's full band on the shield: {before} -> {after}"
    );
}
