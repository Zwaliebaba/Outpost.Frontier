//! The population (work package P3.6d, decision record 0008 points 7 and
//! 10): the load-time draws of a system's full load against the oracle's
//! `load_rand.jsonl`, and SpacePop's run time on the shipped data.
//!
//! **The load-time draws.** `Oracle.exe sim` records every `rand()` from
//! its `srand(seed)` to the first frame (`design/sim-records.md`,
//! `load_rand.jsonl`). Its `load` phase is `StarSystem::load`, whose
//! `SystemLoaded` reaches SpaceSupport's `FSystemInfo::LoadFull`: the
//! zones' validation (`FZonePopulation::Validate`), the trade lanes'
//! rebuild (`CollectSpacePopZones`) and the solar NPCs' personality states
//! (`pub::AI::SubmitState`), 4,837 draws in he01 and 2,390 in ve03
//! (`design/reference-build.md`). The port starts the oracle's server
//! (Content's three controllers, then `LoadGame` over `InitialWorld.ini`),
//! loads the system through the server ([`world::load`]) with the load
//! trace on (`ContentState::draw_marks`) and counts the draws of each
//! step. Without `OUTPOST_ORACLE_DIR` it checks the recorded totals; with
//! it, each scenario run in a system the port loads is compared draw by
//! draw with the oracle's `load` phase: the same count, and the step of
//! each draw (the oracle's by the caller its record names) up to the
//! first difference, reported.
//!
//! Nothing these tests read or write is committed; they return early when
//! `outdir/` is missing.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::content::players::{PlayerShip, PlayerState};
use game_logic::content::systems::DrawMark;
use game_logic::events::ControllerMessage;
use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::scenario::records::{Json, find_files, read_lines};
use game_sim::scenario::{parse, scenario_files};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer world over `outdir/`, `None` when it is missing.
fn world() -> Option<World> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    Some(
        World::new(Arc::new(data), WorldConfig::default())
            .with_assets(Assets::new(root.join("data"))),
    )
}

/// The oracle's start-up: Content's three controllers (and `extra`), then
/// `Server_LoadWorldState` over `InitialWorld.ini`, then the server's
/// world.
fn start(cx: &mut Cx<'_>, extra: &[&str]) {
    let mut names = vec![
        ("GameSupport", Priority::Highest),
        ("SpaceSupport", Priority::Normal),
        ("BaseSupport", Priority::Normal),
    ];
    names.extend(extra.iter().map(|n| (*n, Priority::Normal)));
    for (name, priority) in names {
        let id = controller::create(
            cx,
            b"Content.dll",
            name.as_bytes(),
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{name}");
    }
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    selection::load_world_state(cx, &mut reader);
    world::initialize(cx);
}

/// The draws from `seed` to the generator `now`.
fn draws_since(seed: u32, now: &CrtRand) -> usize {
    let mut r = CrtRand::with_seed(seed);
    for n in 0..20_000_000 {
        if &r == now {
            return n;
        }
        r.rand();
    }
    usize::MAX
}

/// The steps of one system's full load: each step with the indices of its
/// draws from the seed, `[first, end)`.
fn load_steps(seed: u32, marks: &[DrawMark]) -> Vec<(&'static str, usize, usize)> {
    let mut out = Vec::new();
    for pair in marks.windows(2) {
        let first = draws_since(seed, &pair[0].rand);
        let end = draws_since(seed, &pair[1].rand);
        out.push((pair[0].step, first, end));
    }
    out
}

/// The port's draws while system `name` loads through the server, from
/// `seed` set and `skip` draws made just before (the oracle's controller
/// phase): the total and the steps, indices counted from the load.
fn port_load(
    name: &str,
    seed: u32,
    skip: usize,
) -> Option<(usize, Vec<(&'static str, usize, usize)>)> {
    let mut world = world()?;
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    start(&mut cx, &[]);
    cx.world.rng().seed(seed);
    for _ in 0..skip {
        cx.world.rng().rand();
    }
    cx.logic.content.state.draw_marks = Some(Vec::new());
    world::load(&mut cx, create_id(name.as_bytes()));
    let total = draws_since(seed, cx.world.rng()) - skip;
    let marks = cx.logic.content.state.draw_marks.take().unwrap_or_default();
    let steps = load_steps(seed, &marks)
        .into_iter()
        .map(|(s, a, b)| (s, a - skip, b - skip))
        .collect();
    Some((total, steps))
}

/// The draws of each kind of step.
fn per_step(steps: &[(&'static str, usize, usize)]) -> Vec<(&'static str, usize)> {
    let mut out: Vec<(&'static str, usize)> = Vec::new();
    for &(step, first, end) in steps {
        match out.iter_mut().find(|(s, _)| *s == step) {
            Some(entry) => entry.1 += end - first,
            None => out.push((step, end - first)),
        }
    }
    out
}

#[test]
fn the_load_draws_match_the_recorded_counts() {
    // `design/reference-build.md` ("The load-time draws"): 4,837 in he01
    // (1,082 validating the zones, 78 rebuilding the lanes, 3,677 for the
    // 112 solar NPCs' states) and 2,390 in ve03.
    let Some((total, steps)) = port_load("he01", 1, 0) else {
        println!("outdir/ is missing: no system was loaded");
        return;
    };
    let counts = per_step(&steps);
    println!("he01: {total} draws, {counts:?}");
    assert_eq!(total, 4837);
    assert_eq!(
        counts,
        vec![("validate", 1082), ("lanes", 78), ("solar", 3677)]
    );
    // The solar NPCs that get a state graph draw; the others none.
    let solars = steps.iter().filter(|s| s.0 == "solar" && s.2 > s.1).count();
    assert_eq!(solars, 112);
    let (total, _) = port_load("ve03", 1, 0).expect("ve03");
    assert_eq!(total, 2390);
}

/// The step an oracle draw belongs to, by the caller its record names
/// (`fn`, `line` and the frames): `validate`, `lanes`, `solar`, or `?`.
fn oracle_step(draw: &Json) -> &'static str {
    let mut text = String::new();
    for key in ["fn", "line", "at"] {
        if let Some(s) = draw.get(key).and_then(Json::as_str) {
            text.push_str(s);
            text.push(' ');
        }
    }
    if let Some(Json::Array(frames)) = draw.get("stack") {
        for f in frames {
            if let Some(s) = f.as_str() {
                text.push_str(s);
                text.push(' ');
            }
        }
    }
    if text.contains("Validate") || text.contains("EncounterParams.cpp") {
        "validate"
    } else if text.contains("RebuildTradeLanes") || text.contains("CollectSpacePopZones") {
        "lanes"
    } else if text.contains("SubmitPersonalityState") || text.contains("NpcCreate.cpp") {
        "solar"
    } else {
        "?"
    }
}

/// The oracle's `load` phase of a run: its first index, the indices it
/// spans and every draw of it the hooks saw, by index.
fn oracle_load(file: &Path) -> Option<(usize, usize, Vec<(usize, Json)>)> {
    let records = read_lines(file);
    let phase = records.iter().find(|r| {
        r.get("rec").and_then(Json::as_str) == Some("phase")
            && r.get("phase").and_then(Json::as_str) == Some("load")
    })?;
    let first = phase.get("first")?.as_u32()? as usize;
    let count = phase.get("count")?.as_u32()? as usize;
    let draws = records
        .into_iter()
        .filter(|r| {
            r.get("rec").and_then(Json::as_str) == Some("draw")
                && r.get("phase").and_then(Json::as_str) == Some("load")
        })
        .filter_map(|r| Some((r.get("i")?.as_u32()? as usize, r)))
        .collect();
    Some((first, count, draws))
}

#[test]
fn the_load_draws_equal_the_oracle() {
    let Some(dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!("OUTPOST_ORACLE_DIR is not set: the load-time draws were not compared");
        return;
    };
    if world().is_none() {
        println!("outdir/ is missing: the load-time draws were not compared");
        return;
    }
    let mut summary = String::new();
    let mut failures = Vec::new();
    let mut compared = 0;
    for path in scenario_files() {
        let Some(s) = parse(&path) else {
            continue;
        };
        let Some(file) = find_files(&dir.join(&s.name), "load_rand.jsonl")
            .into_iter()
            .min()
        else {
            continue;
        };
        let Some((first, theirs, draws)) = oracle_load(&file) else {
            continue;
        };
        let Some((total, steps)) = port_load(&s.system, s.seed, first) else {
            continue;
        };
        let _ = write!(
            summary,
            "{} ({}): load draws: rust {total}, oracle {theirs}",
            s.name, s.system
        );
        compared += 1;
        let mut difference = None;
        for (at, draw) in &draws {
            let index = at - first;
            let ours = steps
                .iter()
                .find(|(_, a, b)| index >= *a && index < *b)
                .map_or("past the end", |s| s.0);
            let theirs = oracle_step(draw);
            if theirs != "?" && theirs != ours {
                difference = Some((index, ours, theirs));
                break;
            }
        }
        match difference {
            Some((index, ours, theirs)) => {
                let _ = writeln!(
                    summary,
                    "; first difference at draw {index}: rust {ours}, oracle {theirs}"
                );
            }
            None => {
                let _ = writeln!(summary, "; every draw in the same step");
            }
        }
        if total != theirs {
            failures.push(format!("{}: rust {total}, oracle {theirs}", s.name));
        }
    }
    let _ = writeln!(summary, "compared {compared} runs");
    print!("{summary}");
    let _ = std::fs::write(dir.join("rust-load-draws.txt"), &summary);
    assert!(failures.is_empty(), "load-time draws differ: {failures:?}");
}

/// A player with a ship at `position` in system `name`, as GameSupport's
/// `SpaceEnter` leaves its state (the ship spawned by the world, the
/// player in the system's record): the player id and the ship's id.
fn synthetic_player(cx: &mut Cx<'_>, name: &str, position: Vector) -> (u32, u32) {
    let system = create_id(name.as_bytes());
    let spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        position,
        Matrix::IDENTITY,
    );
    let h = cx
        .world
        .spawn_ship(system, &spawn)
        .expect("the player's ship");
    let ship = cx.world.get(h).map_or(0, |o| o.simple.id);
    let player = 1;
    let mut state = PlayerState::new(cx, player);
    state.ship = Some(PlayerShip::new(
        ship,
        create_id(b"he_fighter"),
        Vec::new(),
        Some(system),
    ));
    state.system = Some(system);
    cx.logic.content.state.players.insert(player, state);
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        record.add_player(player);
    }
    (player, ship)
}

/// The NPC ships SpacePop owns.
fn space_pop_ships(cx: &Cx<'_>) -> usize {
    let pop = cx.logic.content.state.space_pop;
    cx.logic
        .content
        .state
        .npc_ships
        .values()
        .filter(|s| s.is_space_pop_ship(pop))
        .count()
}

#[test]
fn space_pop_populates_he01_around_a_player() {
    // A player launches beside Station he01_11 (its zone is populated, density
    // 12) on a multiplayer server with SpacePop: the arrival population
    // (`SpaceEnter`), then twenty heartbeats. The run draws from one seed,
    // so it repeats exactly; its ships are SpacePop's.
    let run = || {
        let mut world = world()?;
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        start(&mut cx, &["SpacePop"]);
        let system = create_id(b"he01");
        world::load(&mut cx, system);
        cx.world.rng().seed(7);
        let (player, _) = synthetic_player(&mut cx, "he01", Vector::new(5541.0, 0.0, -49500.0));
        let pop = cx.logic.content.state.space_pop;
        let _ = cx.dispatch(
            pop,
            &mut ControllerMessage::SpaceEnter {
                system,
                player,
                launch_object: 0,
                launch_kind: -1,
            },
        );
        let arrival = space_pop_ships(&cx);
        for _ in 0..20 {
            let _ = cx.dispatch(pop, &mut ControllerMessage::Heartbeat { elapsed: 3.0 });
        }
        let after = space_pop_ships(&cx);
        let draws = draws_since(7, cx.world.rng());
        let names: Vec<String> = cx
            .logic
            .content
            .state
            .npc_ships
            .values()
            .map(|s| String::from_utf8_lossy(&s.nickname).into_owned())
            .collect();
        Some((arrival, after, draws, names))
    };
    let Some(first) = run() else {
        println!("outdir/ is missing: SpacePop was not run");
        return;
    };
    println!(
        "he01: {} ships on arrival, {} after 20 heartbeats, {} draws; {:?}",
        first.0, first.1, first.2, first.3
    );
    assert!(first.0 > 0, "the arrival population made no ship");
    assert!(first.1 >= first.0);
    assert_eq!(run(), Some(first), "the population does not repeat");
}

#[test]
fn the_shipped_encounters_stay_in_bounds() {
    // The reads the C++ makes without a test (D1060, D1061, D1063): after
    // every system's full load, each validated zone has a current
    // permutation, every permutation entry names a formation, a formation
    // tied to another names one, a line's npc archetype exists, and a
    // patrol formation's zone has a path with segments in the zone.
    let Some(mut world) = world() else {
        println!("outdir/ is missing: the encounters were not checked");
        return;
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    start(&mut cx, &[]);
    let systems: Vec<u32> = cx.logic.server_world.systems.keys().copied().collect();
    for &system in &systems {
        world::load(&mut cx, system);
    }
    let tables = cx.logic.content.tables.clone();
    let mut problems = Vec::new();
    let mut encounters = 0;
    for (id, record) in &cx.logic.content.state.systems {
        for zone in &record.zones {
            for e in &zone.population.encounters {
                encounters += 1;
                let p = &e.parameters;
                let at = format!(
                    "system {id:#x} zone {:#x} encounter {:#x}",
                    zone.zone_id, p.nickname
                );
                if zone.population.valid && p.current_permutation.is_none() {
                    problems.push(format!("{at}: no current permutation"));
                }
                if p.formations.is_empty() {
                    problems.push(format!("{at}: no formation"));
                }
                for perm in &p.permutations {
                    for entry in &perm.entries {
                        if usize::try_from(entry.formation_index)
                            .ok()
                            .is_none_or(|i| i >= p.formations.len())
                        {
                            problems.push(format!("{at}: entry {}", entry.formation_index));
                        }
                    }
                }
                for f in &p.formations {
                    if f.feeling_to_formation == 1
                        && usize::try_from(f.feeling_value)
                            .ok()
                            .is_none_or(|i| i >= p.formations.len())
                    {
                        problems.push(format!("{at}: feeling_to_formation {}", f.feeling_value));
                    }
                    for line in &f.ships {
                        if line.npc_arch != 0 && tables.npc_ship(line.npc_arch).is_none() {
                            problems.push(format!("{at}: npc arch {:#x}", line.npc_arch));
                        }
                    }
                    if f.behavior == 3 {
                        let segments = zone
                            .path
                            .and_then(|i| record.paths.get(i))
                            .and_then(|path| path.zone_segments(zone.zone_id))
                            .unwrap_or_default();
                        if segments.is_empty() {
                            problems.push(format!("{at}: a patrol without a path"));
                        }
                    }
                }
            }
        }
    }
    println!(
        "{} systems, {encounters} zone encounters, {} problems",
        systems.len(),
        problems.len()
    );
    assert!(encounters > 0);
    assert!(problems.is_empty(), "{problems:#?}");
}
