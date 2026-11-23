//! GameSupport's run time and Content's system records (work package
//! P3.4d, decision record 0008 point 10).
//!
//! The port starts a multiplayer server as `Oracle.exe sim` does for
//! `CppSource/Oracle/system_loads.txt`: Content's GameSupport, SpaceSupport
//! and BaseSupport (the oracle's reduced `[Initial MP DLLs]`), then
//! `Server_LoadWorldState` over `InitialWorld.ini` (`LoadGame`: the base
//! records, the gate locks, the system records, the system paths), then
//! he01 and every system loaded ([`world::load`], whose `SystemLoaded`
//! runs each record's full load in SpaceSupport). It renders what
//! `Content.dll`'s `OracleDumpContentSystems`
//! (`CppSource/Content/oracle_content_systems.cpp`) writes:
//!
//! - `content_systems`: each record's counts, load count, local faction
//!   and solar detection;
//! - `content_system_jumps`, `content_system_lanes` (the live rings with
//!   their attack zones) and `content_solar_npcs` (each NPC's vibe and the
//!   affiliation it has now: fact F17);
//! - `content_gate_locks`: the `[locked_gates]` writer;
//! - `content_mplayer`: the `[mPlayer]` writer on a synthetic state, then
//!   the same text read back and written again.
//!
//! `content_systems_equal_the_oracle` compares them with the oracle's files in
//! `OUTPOST_ORACLE_DIR` (the data job's system-loads output); without it
//! the port's lines are rendered from `outdir/` and nothing is compared.
//! The other tests check the shipped data in `outdir/` and return early
//! when it is missing. Nothing these tests read or write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::players::{self, PlayerState};
use game_logic::content::systems::SystemInfo;
use game_logic::events::ControllerMessage;
use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

/// The tables in the oracle's order, each `content_<name>.txt`.
const TABLES: [&str; 6] = [
    "systems",
    "system_jumps",
    "system_lanes",
    "solar_npcs",
    "gate_locks",
    "mplayer",
];

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn vector(v: Vector) -> String {
    format!("{},{},{}", bits(v.x), bits(v.y), bits(v.z))
}

fn rotation(m: &Matrix, separator: &str) -> String {
    m.m.iter()
        .flatten()
        .map(|v| bits(*v))
        .collect::<Vec<_>>()
        .join(separator)
}

/// A multiplayer world over `outdir/` with its data root.
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

/// The oracle's start-up: Content's three controllers, then
/// `Server_LoadWorldState` over `InitialWorld.ini`.
fn start(cx: &mut Cx<'_>) {
    for (name, priority) in [
        ("GameSupport", Priority::Highest),
        ("SpaceSupport", Priority::Normal),
        ("BaseSupport", Priority::Normal),
    ] {
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
}

/// he01, then every system, as the oracle loads them.
fn load_every_system(cx: &mut Cx<'_>) {
    world::initialize(cx);
    world::load(cx, create_id(b"he01"));
    let systems: Vec<u32> = cx.logic.server_world.systems.keys().copied().collect();
    for system in systems {
        world::load(cx, system);
    }
}

/// The synthetic player state of `FillPlayerState`.
fn synthetic_player(cx: &mut Cx<'_>) -> PlayerState {
    let base = api::misc::base_id(cx, b"he01_01_Base").unwrap_or(0);
    let he02 = api::misc::system_id(cx, b"He02").unwrap_or(0);
    let he01 = api::misc::system_id(cx, b"He01").unwrap_or(0);
    let mut state = PlayerState::new(cx, 7);
    state.vnpcs = vec![
        [base, create_id(b"he0101_fix_bartender"), 3, 1],
        [base, 0x8000_0001, 0, 2],
    ];
    state.rumors = vec![[5, 2], [6, 1]];
    state.can_dock = false;
    state.dock_exceptions = vec![11, 12];
    state.can_tradelane = true;
    state.tradelane_exceptions = vec![[21, 22]];
    let stats = &mut state.stats;
    stats.ship_types_killed.insert(200, 1);
    stats.ship_types_killed.insert(100, 3);
    stats.random_missions_completed.insert(1, 2);
    stats.random_missions_aborted.insert(0, 1);
    stats.random_missions_failed.insert(5, 4);
    stats.total_cash_earned = 12345.678;
    stats.total_time_played = 3600.25;
    stats.systems_visited = vec![he02, he01];
    stats.bases_visited = vec![base];
    stats.holes_visited = vec![77];
    state
}

/// `DumpPlayerState`: the first save, then the save of the state read back.
fn player_text(cx: &mut Cx<'_>) -> String {
    let first = synthetic_player(cx);
    let locks = cx.logic.content.state.gate_locks.clone();
    let mut text = Vec::new();
    first.save(cx, &locks, &mut text).expect("write");
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    let mut second = PlayerState::new(cx, 7);
    players::load_player_state(cx, &mut second, &mut reader);
    let locks = cx.logic.content.state.gate_locks.clone();
    let mut again = Vec::new();
    second.save(cx, &locks, &mut again).expect("write");
    text.extend_from_slice(&again);
    String::from_utf8_lossy(&text).into_owned()
}

fn system_line(id: u32, s: &SystemInfo) -> String {
    format!(
        "system={id} loads={} local_faction={} rpop={} zones={} paths={} jumps={} lanes={} \
         lane_rings={} solar_npcs={} players={} mission_objects={}",
        s.load_count,
        s.local_faction,
        u32::from(s.rpop_solar_detection),
        s.zones.len(),
        s.paths.len(),
        s.jumps.len(),
        s.trade_lanes.len(),
        s.lane_by_ring.len(),
        s.solar_npcs.len(),
        s.players.len(),
        s.mission_objects.len()
    )
}

/// The six tables, rendered.
fn render(cx: &mut Cx<'_>) -> Vec<Vec<String>> {
    let mut out: Vec<Vec<String>> = vec![Vec::new(); TABLES.len()];
    let systems = cx.logic.content.state.systems.clone();
    for (&id, s) in &systems {
        out[0].push(system_line(id, s));
        for j in &s.jumps {
            out[1].push(format!(
                "system={id} object={} type={} pos={} goto_system={} goto_object={}",
                j.object,
                j.kind,
                vector(j.position),
                j.goto_system,
                j.goto_object
            ));
        }
        for (index, lane) in s.trade_lanes.iter().enumerate() {
            let rings: Vec<String> = lane
                .rings
                .iter()
                .map(|r| {
                    format!(
                        "{}:{}:{}:{}:{}:{}",
                        r.object,
                        bits(r.position.x),
                        bits(r.position.y),
                        bits(r.position.z),
                        r.reputation,
                        rotation(&r.rotation, ":")
                    )
                })
                .collect();
            let candidates: Vec<String> = lane
                .ring_candidates
                .iter()
                .map(|c| {
                    let zones: Vec<String> = c
                        .candidates
                        .iter()
                        .map(|&(z, w)| format!("{}@{}", s.zones[z].zone_id, bits(w)))
                        .collect();
                    format!(
                        "{}:{}:{}",
                        c.ring_index,
                        bits(c.max_weight),
                        zones.join("/")
                    )
                })
                .collect();
            out[2].push(format!(
                "system={id} lane={index} start={} end={} rotation={} max_weight={} rings={} candidates={}",
                vector(lane.start),
                vector(lane.end),
                rotation(&lane.rotation, ","),
                bits(lane.max_weight),
                rings.join("+"),
                candidates.join("+")
            ));
        }
        for (index, npc) in s.solar_npcs.iter().enumerate() {
            let tail = match npc.reputation {
                Some(vibe) => {
                    let affiliation = api::reputation::affiliation(cx, vibe).unwrap_or(u32::MAX);
                    format!("vibe={vibe} affiliation={affiliation}")
                }
                None => "vibe=none affiliation=none".to_owned(),
            };
            out[3].push(format!(
                "system={id} index={index} object={} nickname={} base={} msg_id_prefix={} \
                 state_graph={} pilot={} {tail}",
                npc.object,
                hex(&npc.nickname),
                npc.base.unwrap_or(0),
                hex(&npc.msg_id_prefix),
                npc.state_graph,
                u32::from(npc.has_personality)
            ));
        }
    }
    let mut locks = Vec::new();
    cx.logic
        .content
        .state
        .gate_locks
        .save(&mut locks)
        .expect("write");
    out[4] = String::from_utf8_lossy(&locks)
        .lines()
        .map(str::to_owned)
        .collect();
    out[5] = player_text(cx).lines().map(str::to_owned).collect();
    out
}

/// The port's server after the oracle's start-up and loads.
fn loaded() -> Option<(World, Logic)> {
    let mut world = world()?;
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    start(&mut cx);
    load_every_system(&mut cx);
    Some((world, logic))
}

/// The differences of one table from the oracle's file.
fn compare(table: &str, oracle: &str, rendered: &[String]) -> Vec<String> {
    let mut problems = Vec::new();
    let want: Vec<&str> = oracle.lines().collect();
    if want.len() != rendered.len() {
        problems.push(format!(
            "{table}: rust has {} lines, the oracle {}",
            rendered.len(),
            want.len()
        ));
    }
    let cut = |s: &str| s.chars().take(400).collect::<String>();
    let mut shown = 0;
    for (i, (w, g)) in want.iter().zip(rendered).enumerate() {
        if *w != g {
            problems.push(format!(
                "{table} line {}:\n  oracle: {}\n  rust:   {}",
                i + 1,
                cut(w),
                cut(g)
            ));
            shown += 1;
            if shown == 3 {
                break;
            }
        }
    }
    problems
}

#[test]
fn content_systems_equal_the_oracle() {
    let Some((mut world, mut logic)) = loaded() else {
        println!("notice: outdir/ is missing; the content systems dump was skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let rendered = render(&mut cx);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "content systems: {} tables, {} lines",
        rendered.len(),
        rendered.iter().map(Vec::len).sum::<usize>()
    );
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
    }
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let mut problems = Vec::new();
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let path = oracle.join(format!("content_{table}.txt"));
        match std::fs::read(&path) {
            Ok(bytes) => problems.extend(compare(table, &String::from_utf8_lossy(&bytes), lines)),
            Err(e) => problems.push(format!("{}: {e}", path.display())),
        }
    }
    let _ = writeln!(
        summary,
        "{} tables compared with the oracle, {} differences",
        rendered.len(),
        problems.len()
    );
    for p in problems.iter().take(30) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-content-systems.txt"), &summary);
    assert!(
        problems.is_empty(),
        "content systems: {} differences from the oracle",
        problems.len()
    );
}

/// Counts of the shipped data, from the files independently, and F17: a
/// named object with a `reputation` has its faction once its system has
/// loaded.
#[test]
fn system_records_on_outdir() {
    let Some((mut world, mut logic)) = loaded() else {
        println!("outdir/ is missing: no system records");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let systems = cx.logic.content.state.systems.clone();
    let universe = cx.world.data().universe.systems.len();
    assert_eq!(systems.len(), universe);
    let npcs: usize = systems.values().map(|s| s.solar_npcs.len()).sum();
    let with_rep: usize = systems
        .values()
        .flat_map(|s| &s.solar_npcs)
        .filter(|n| n.reputation.is_some())
        .count();
    let lanes: usize = systems.values().map(|s| s.trade_lanes.len()).sum();
    let jumps: usize = systems.values().map(|s| s.jumps.len()).sum();
    println!(
        "{} records, {npcs} solar NPCs ({with_rep} with a reputation), {lanes} lanes, {jumps} jumps",
        systems.len()
    );
    assert!(with_rep > 0);
    // Every handle's vibe has the NPC's faction.
    let mut wrong = 0;
    for npc in systems.values().flat_map(|s| &s.solar_npcs) {
        if let Some(vibe) = npc.reputation {
            let affiliation = api::reputation::affiliation(&mut cx, vibe).unwrap_or(u32::MAX);
            if affiliation != npc.reputation_group {
                wrong += 1;
            }
        }
    }
    assert_eq!(wrong, 0, "solar NPCs whose vibe lacks their faction");
    // he01's lanes chain rings and every ring is in the ring map.
    let he01 = &systems[&create_id(b"he01")];
    assert_eq!(he01.load_count, 1);
    assert!(!he01.trade_lanes.is_empty());
    for lane in &he01.trade_lanes {
        for ring in &lane.rings {
            assert!(he01.lane_of_ring(ring.object).is_some());
        }
    }
    // InitialWorld.ini's `[locked_gates]`, locked for every player.
    let gates = cx.logic.content.state.gate_locks.collect(0);
    assert!(!gates.is_empty());
    assert!(cx.logic.content.state.gate_locks.is_locked(3, gates[0]));
}

/// `[mPlayer]` round trip, Rust write to Rust read: the text written
/// again is the first one, and the state read back is the state written.
#[test]
fn mplayer_round_trip() {
    let Some(mut world) = world() else {
        println!("outdir/ is missing: no [mPlayer] round trip");
        return;
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    start(&mut cx);
    let first = synthetic_player(&mut cx);
    let text = player_text(&mut cx);
    let half = text.len() / 2;
    assert_eq!(&text[..half], &text[half..], "the second save");
    let mut reader = IniReader::new();
    reader.open_memory(&text.as_bytes()[..half]);
    let mut back = PlayerState::new(&cx, 7);
    players::load_player_state(&mut cx, &mut back, &mut reader);
    assert_eq!(back, first);
    assert!(text.starts_with("[mPlayer]\nlocked_gate = "));
    assert!(text.contains("vnpc = "));
    assert!(text.contains("total_cash_earned = 12345.677734\n"));
}

/// GameSupport's player cases through the bus: `PlayerLoad` makes the
/// state from `[mPlayer]`, `PlayerSave` writes it, `PlayerLogout` drops
/// it; the heartbeat adds the play time.
#[test]
fn game_support_keeps_the_player_states() {
    let Some(mut world) = world() else {
        println!("outdir/ is missing: no player states");
        return;
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    start(&mut cx);
    let mut reader = IniReader::new();
    reader.open_memory(
        b"[Player]\nname = x\n[mPlayer]\ncan_dock = 0\ndock_exception = 9\nrumor = 4, 2\n\
          total_time_played = 10.5\nsys_visited = 5\nsys_visited = 5\n",
    );
    let mut load = ControllerMessage::PlayerLoad {
        player: 3,
        reader: &mut reader,
    };
    let _ = cx.dispatch(controller::ControllerId::BROADCAST, &mut load);
    let state = cx.logic.content.state.players[&3].clone();
    assert!(!state.can_dock);
    assert_eq!(state.dock_exceptions, [9]);
    assert_eq!(state.rumors, [[4, 2]]);
    assert_eq!(state.stats.systems_visited, [5]);
    game_logic::content::game_support::heartbeat(&mut cx, 1.0);
    assert_eq!(
        cx.logic.content.state.players[&3].stats.total_time_played,
        11.5
    );
    let mut out = Vec::new();
    let mut save = ControllerMessage::PlayerSave {
        client: 3,
        file: &mut out,
    };
    let _ = cx.dispatch(controller::ControllerId::BROADCAST, &mut save);
    let text = String::from_utf8_lossy(&out);
    assert!(text.contains("can_dock = 0\ndock_exception = 9\n"));
    assert!(text.contains("total_time_played = 11.500000\n"));
    let mut logout = ControllerMessage::PlayerLogout { player: 3 };
    let _ = cx.dispatch(controller::ControllerId::BROADCAST, &mut logout);
    assert!(!cx.logic.content.state.players.contains_key(&3));
    // `SaveGame` writes the gate locks of `InitialWorld.ini`.
    let mut out = Vec::new();
    let mut save = ControllerMessage::SaveGame { file: &mut out };
    let _ = cx.dispatch(controller::ControllerId::BROADCAST, &mut save);
    let text = String::from_utf8_lossy(&out);
    assert!(text.starts_with("[locked_gates]\nnpc_locked_gate = "));
    assert!(text.contains("\nlocked_gate = "));
}
