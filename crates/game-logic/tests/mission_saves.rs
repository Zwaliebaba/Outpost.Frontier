//! The story missions' and Story's player-save sections (work package
//! P3.5b, decision record 0008 points 6 and 10, Gate G3): the
//! `[MissionState]`, `[TriggerSave]`, mission object and `[StoryInfo]`
//! sections of `game_logic::content::mission::{save, object_save}` and
//! `game_logic::content::story`, against `Content.dll`'s
//! `OracleDumpMissionSaves` and `OracleLoadMissionSaves`
//! (`CppSource/Content/oracle_save_dump.cpp`).
//!
//! The port starts a multiplayer server as the oracle does, registers
//! player 7 in he01 and makes each mission as `mission_scripts.rs` does
//! (dialogs emptied, the generator seeded, a controller id no controller
//! has). Each mission is saved in three states (`constructed`, `advanced`,
//! `pending`; the recipes are `advance` and `pending` below and
//! `Advance` in the export, described in `design/sim-records.md`).
//!
//! - `data_dump_mission_saves` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   the port's save text of every mission and state, and of a new Story,
//!   against the C++ writer's (`mission_saves/`). Summary
//!   `rust-mission-saves.txt`.
//! - `rust_saves_for_the_oracle` (`OUTPOST_DATA_DIR`,
//!   `OUTPOST_SAVE_LOADS_DIR`): writes the port's saves, as the server's
//!   text-mode save path writes them, into `<dir>/rust/` for
//!   `Oracle.exe sim CppSource/Oracle/mission_save_loads.txt <dir>`: the
//!   three states, the restore of the hand-made `tests/fixtures/
//!   mission_saves/raw.ini` saved again (`restored`), the fixture itself
//!   (`raw`), and six Story saves made from `story_<n>.ini`.
//! - `oracle_loads_of_rust_saves` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`
//!   set to that `<dir>`): the port restores the same files and renders the
//!   restored state as the export does; compared with `loaded_*.txt` and
//!   `story_loaded_*.txt`. Summary `rust-mission-save-loads.txt`.
//! - `save_round_trips_on_outdir` and `the_raw_fixture_restores_on_outdir`
//!   run on `outdir/` and return early without it; the Story tests need no
//!   data.
//!
//! Without its variables a data test prints a notice and passes. Nothing
//! these tests write is committed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::mission::condition::Condition;
use game_logic::content::mission::field::hex;
use game_logic::content::mission::object_save::{
    Invulnerable, RandEncRespawn, VibeInfo, local_objective,
};
use game_logic::content::mission::rand_enc::ShipChoice;
use game_logic::content::mission::save::{self, AmbientScene, RtcScript};
use game_logic::content::mission::{self, CHAPTERS, MissionScript};
use game_logic::content::players::{self, PlayerState};
use game_logic::content::story::{self, Story, StoryLoadStep};
use game_logic::selection;
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::msvcrt::{text_mode_read, text_mode_write};
use neuron_core::num::trunc_to_i64;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// `OracleSaveSeed`.
const SEED: u32 = 0x4f52_4143;
/// `OracleSavePlayerId`.
const PLAYER: u32 = 7;
/// `OracleSaveControllerBase`.
const CONTROLLER_BASE: u32 = 0x00ff_0001;
/// The states the C++ writer writes.
const STATES: [&str; 3] = ["constructed", "advanced", "pending"];
/// The states the C++ loads.
const LOAD_STATES: [&str; 5] = ["constructed", "advanced", "pending", "restored", "raw"];
/// `OracleSaveFloats`.
const FLOATS: [f32; 12] = [
    0.007_812_5,
    0.023_437_5,
    0.1,
    123.456,
    -0.6,
    31_476.25,
    3.402_823_5e38,
    1e-7,
    2500.0,
    0.333_333_34,
    16_777_216.0,
    -0.0,
];
/// `OracleSaveSystems`.
const SYSTEMS: [u32; 3] = [7, 16_777_217, 4_000_000_007];
/// `OracleSaveStoryCount`.
const STORIES: usize = 6;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn fixture(name: &str) -> Vec<u8> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/mission_saves")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

/// The kind of each of [`CHAPTERS`].
fn kind(index: usize) -> ControllerKind {
    match index {
        0 => ControllerKind::Mission01a,
        1 => ControllerKind::Mission01b,
        n => ControllerKind::Mission(u8::try_from(n).expect("a chapter index")),
    }
}

/// A multiplayer server over a data root, started as the oracle starts it.
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

/// The server over the dump root of `OUTPOST_DATA_DIR`, or over `outdir/`.
fn server(root: Option<&Path>) -> Option<(World, Logic)> {
    match root {
        Some(root) => started(&root.join("EXE"), &root.join("DATA")),
        None => {
            let root = outdir();
            started(&root, &root.join("data"))
        }
    }
}

/// `RegisterOraclePlayer`: player 7 in he01.
fn register_player(cx: &mut Cx<'_>) {
    let he01 = api::misc::system_id(cx, b"he01").ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = he01.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
}

/// `MakeOracleMission`.
fn make(cx: &mut Cx<'_>, index: usize) -> MissionScript {
    cx.logic.content.state.mission_dialogs.clear();
    cx.world.rng().seed(SEED);
    let mut script = mission::construct_script(
        cx,
        ControllerId(CONTROLLER_BASE + u32::try_from(index).unwrap()),
        kind(index),
        &CreateParams::StoryMission {
            player: PLAYER,
            story: ControllerId::BROADCAST,
        },
    );
    for trigger in script.triggers.values_mut() {
        match trigger.condition.as_mut() {
            Some(Condition::Destroyed(c)) => c.count = 0,
            Some(Condition::LootAcquired(c)) => c.count = 0,
            _ => {}
        }
    }
    script
}

fn float(index: usize) -> f32 {
    FLOATS[index % FLOATS.len()]
}

/// `AdvanceCondition`.
fn advance_condition(condition: &mut Condition, k: usize) {
    match condition {
        Condition::Destroyed(c) => c.count = i32::try_from(k + 1).unwrap(),
        Condition::LootAcquired(c) => c.count = i32::try_from(k + 1).unwrap(),
        Condition::ProjHit(c) => c.count = i32::try_from(k + 1).unwrap(),
        Condition::ProjHitShipToLbl(c) => c.count = i32::try_from(k + 1).unwrap(),
        Condition::DistShip(c) => {
            c.tick_timer = float(k);
            c.last_distance_sq = float(k + 1);
        }
        Condition::DistVec(c) => {
            c.tick_timer = float(k);
            c.last_distance_sq = float(k + 1);
        }
        Condition::DistVecLbl(c) => c.tick_timer = float(k + 2),
        Condition::TetherBroke(c) => {
            c.field_0x90 = float(k + 3);
            c.field_0x88 = float(k + 4);
        }
        Condition::Timer(c) => c.elapsed = float(k + 5),
        Condition::NpcSystemEnter(c) => {
            c.pending.clone_from(&c.ships);
            for (i, entry) in c.pending.iter_mut().enumerate() {
                entry.system_id = SYSTEMS[(k + i) % 3];
            }
        }
        Condition::NpcSystemExit(c) => {
            c.pending.clone_from(&c.ships);
            for (i, entry) in c.pending.iter_mut().enumerate() {
                entry.system_id = SYSTEMS[(k + i) % 3];
            }
        }
        _ => {}
    }
}

/// `Advance`: the advanced state (`design/sim-records.md`).
fn advance(cx: &mut Cx<'_>, s: &mut MissionScript) {
    // Every trigger with a condition, in id order.
    let mut k = 0;
    for (id, trigger) in &mut s.triggers {
        let Some(condition) = trigger.condition.as_mut() else {
            continue;
        };
        s.active_triggers.push(*id);
        advance_condition(condition, k);
        k += 1;
    }
    s.story_cue = 7;
    s.nn_path.active = true;
    s.nn_path.travel_ids = 1234;
    s.nn_path.target_ids = -5;
    s.nn_path.position = Vector::new(1.5, -2.25, 100_000.125);
    s.nn_path.label = b"oracle_nn_target".to_vec();
    s.nn_path.system_id = 4_000_000_007;
    for (path, finished, field_0x95) in [
        (&b"missions\\oracle\\rtc_a.thn"[..], false, true),
        (b"missions\\oracle\\rtc_b.thn", true, false),
        (b"missions\\oracle\\rtc_c.thn", false, false),
    ] {
        s.rtc_scripts.push(RtcScript {
            path: path.to_vec(),
            handle: -1,
            started: false,
            finished,
            field_0x95,
        });
    }
    s.ambient_scenes.push(AmbientScene {
        path: b"scripts\\oracle\\ambi.thn".to_vec(),
        handle: -1,
        room_id: 0x1234,
        base_id: 4_000_000_007,
    });
    s.friendly_fire_excused = vec![b"oracle_ff_a".to_vec(), b"oracle_ff_b".to_vec()];

    let o = &mut s.object;
    let first_list = o.obj_lists.values().next().cloned();
    let first_meta = o
        .meta_behaviors
        .values()
        .next()
        .and_then(|l| l.first())
        .cloned();
    if let (Some(front), Some(back)) = (o.ships.first(), o.ships.last()) {
        let mut front = front.clone();
        front.ships.truncate(1);
        front.arrival_obj_id = 0;
        front.position = Vector::new(-31_476.25, 150.0, 0.1);
        if let Some(ship) = front.ships.first_mut() {
            ship.priority = 3;
            ship.hit_points = 450;
            if let Some(list) = &first_list {
                ship.directives.clone_from(list);
            }
        }
        let mut back = back.clone();
        back.ships.truncate(1);
        back.arrival_obj_id = 0x4000_0123;
        if let Some(ship) = back.ships.first_mut() {
            ship.priority = -2;
            ship.guide.clone_from(&first_meta);
        }
        o.respawn_ships.push(front);
        o.respawn_ships.push(back);
    }
    let mut record = RandEncRespawn {
        encounter_id: o
            .rand_encs
            .first()
            .map_or_else(|| create_id(b"oracle_enc"), |e| e.nickname_id),
        count: 2,
        priority: 3,
        hit_points: 450,
        choices: vec![
            ShipChoice {
                ship_classes: vec![0x11, 0x22],
                weight: 1.0,
            },
            ShipChoice {
                ship_classes: Vec::new(),
                weight: 1.0,
            },
        ],
        names: vec![b"oracle_rand_a".to_vec(), b"oracle_rand_b".to_vec()],
        directives: first_list.unwrap_or_default(),
        ..RandEncRespawn::default()
    };
    record.placement.translation = Vector::new(100.5, -0.0, 3_000_000.0);
    o.rand_enc_respawns.push(record);
    o.respawn_solars.push(b"oracle_solar".to_vec());
    o.mark_obj_names = vec![b"oracle_mark_a".to_vec(), b"oracle_mark_b".to_vec()];
    o.decloaked.push(b"oracle_cloak".to_vec());
    o.invulnerables = vec![
        Invulnerable {
            nickname: b"oracle_inv_a".to_vec(),
            duration: 12.5,
            param3: true,
        },
        Invulnerable {
            nickname: b"oracle_inv_b".to_vec(),
            duration: 0.023_437_5,
            param3: false,
        },
    ];
    o.vibe_infos = vec![
        VibeInfo {
            object_a: b"oracle_vibe_a".to_vec(),
            object_b: b"Player".to_vec(),
            rep_a: -1,
            rep_b: -1,
            attitude: -0.6,
        },
        VibeInfo {
            object_a: Vec::new(),
            object_b: b"oracle_vibe_b".to_vec(),
            rep_a: 17,
            rep_b: -1,
            attitude: 0.1,
        },
        VibeInfo {
            object_a: b"oracle_vibe_c".to_vec(),
            object_b: Vec::new(),
            rep_a: -1,
            rep_b: 4_000_000_007_u32 as i32,
            attitude: -0.0,
        },
    ];
    let mut encs = o.rand_encs.iter_mut();
    if let Some(e) = encs.next() {
        e.enabled = true;
        e.npc_count = 3;
    }
    if let Some(e) = encs.next() {
        e.enabled = false;
        e.npc_count = 5;
    }
    let clamps = &mut cx.logic.content.state.clamps;
    clamps.attack_clamp = 1;
    clamps.encounter_enabled = 0;
    clamps.scan_clamp = 1;
    clamps.set_gcs_clamp_enabled(1);
    clamps.hostile_clamp = 0;
    clamps.population_enabled = 0;
}

/// The pending state: a trigger save of the first trigger.
fn pending(s: &mut MissionScript) {
    s.object.trigger_save_pending = true;
    s.save_trigger_id = s.triggers.keys().next().copied().unwrap_or(0);
}

/// `WriteOracleSave`.
fn write_save(cx: &Cx<'_>, s: &mut MissionScript) -> Vec<u8> {
    let mut out = Vec::new();
    save::write_save(cx, s, &mut out).expect("a save in memory");
    out
}

/// The mission of `index` in state `state` (one of [`STATES`]) and its
/// save; the clamps are put back.
fn saved(cx: &mut Cx<'_>, index: usize, state: &str) -> (MissionScript, Vec<u8>) {
    let clamps = cx.logic.content.state.clamps;
    let mut s = make(cx, index);
    match state {
        "advanced" => advance(cx, &mut s),
        "pending" => pending(&mut s),
        _ => {}
    }
    let text = write_save(cx, &mut s);
    cx.logic.content.state.clamps = clamps;
    (s, text)
}

/// The draws from `before` to `after` (at most 0x10000).
fn draws(before: &CrtRand, after: &CrtRand) -> i64 {
    let mut probe = before.clone();
    for n in 0..=0x10000 {
        if probe == *after {
            return n;
        }
        probe.rand();
    }
    -1
}

fn reader_over(bytes: &[u8]) -> IniReader {
    let mut reader = IniReader::new();
    reader.open(b"save.ini", bytes.to_vec());
    reader
}

/// A new mission of `index` with `bytes` restored, the generator seeded
/// before the restore, and the restore's draws.
fn restored(cx: &mut Cx<'_>, index: usize, bytes: &[u8]) -> (MissionScript, i64) {
    let mut s = make(cx, index);
    cx.world.rng().seed(SEED);
    let before = cx.world.rng().clone();
    let mut reader = reader_over(bytes);
    let _ = save::restore(cx, &mut reader, &mut s);
    let after = cx.world.rng().clone();
    (s, draws(&before, &after))
}

/// `DumpLoaded`.
fn render_loaded(cx: &Cx<'_>, s: &mut MissionScript, draws: i64) -> Vec<u8> {
    let mut out = String::new();
    let active: Vec<String> = s
        .initially_active
        .iter()
        .map(|(a, b)| format!("{a}:{b}"))
        .collect();
    let _ = writeln!(
        out,
        "script field_0x2ec={} initially_active={} active={} pending_activations={} save_pending={} save_trigger={} nn_state={} draws={draws}",
        u8::from(s.field_0x2ec),
        active.join(","),
        s.active_triggers.len(),
        s.pending_activations.len(),
        u8::from(s.object.trigger_save_pending),
        s.save_trigger_id,
        s.nn_path.state
    );
    let mut bytes = out.into_bytes();
    for id in s.initially_active.keys() {
        let Some(condition) = s.triggers.get(id).and_then(|t| t.condition.as_ref()) else {
            continue;
        };
        bytes.extend_from_slice(format!("cnd trigger={id} text=").as_bytes());
        condition.save(&mut bytes).expect("in memory");
        bytes.extend_from_slice(b"|\n");
    }
    let mut out = String::new();
    for (g, group) in s.object.respawn_ships.iter().enumerate() {
        for (i, ship) in group.ships.iter().enumerate() {
            let _ = writeln!(
                out,
                "ship group={g} index={i} init_objectives={} init_meta={} guide={} directives={}",
                ship.init_objectives,
                ship.init_meta_behavior,
                u8::from(ship.guide.is_some()),
                ship.directives.len()
            );
        }
    }
    let reps: Vec<String> = s.object.solar_reps.iter().map(i32::to_string).collect();
    let _ = writeln!(out, "solar_reps={}", reps.join(","));
    for e in &s.object.rand_encs {
        let _ = writeln!(
            out,
            "rand_enc nickname={} enabled={} npc_count={} params={}",
            hex(&e.nickname),
            u8::from(e.enabled),
            e.npc_count,
            u8::from(e.params.is_some())
        );
    }
    out.push_str("=== save\n");
    bytes.extend_from_slice(out.as_bytes());
    bytes.extend(write_save(cx, s));
    bytes
}

/// The Story the export makes: its constructor's values, and the player's
/// mission number set to 0 as `SetContentPlayerMissionNumber` does.
fn new_story(cx: &mut Cx<'_>) -> Story {
    players::set_mission_number(cx, PLAYER, 0, false);
    Story::new(PLAYER)
}

fn story_save(cx: &Cx<'_>, story: &Story) -> Vec<u8> {
    let mut out = Vec::new();
    story::save(cx, story, &mut out).expect("in memory");
    out
}

/// The first differing line of two texts, or none.
fn first_difference(name: &str, want: &[u8], got: &[u8]) -> Option<String> {
    let want = String::from_utf8_lossy(want);
    let got = String::from_utf8_lossy(got);
    if want == got {
        return None;
    }
    let mut w = want.split('\n');
    let mut g = got.split('\n');
    let mut line = 1;
    loop {
        match (w.next(), g.next()) {
            (Some(a), Some(b)) if a == b => line += 1,
            (a, b) => {
                return Some(format!(
                    "{name} line {line}: oracle {:?}, port {:?}",
                    a.unwrap_or("<end>"),
                    b.unwrap_or("<end>")
                ));
            }
        }
    }
}

fn data_root() -> Option<PathBuf> {
    std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from)
}

/// The C++ writer against the port's, for every mission, state and the
/// new Story.
#[test]
fn data_dump_mission_saves() {
    let Some(root) = data_root() else {
        println!("notice: OUTPOST_DATA_DIR is not set; the mission saves data dump was skipped");
        return;
    };
    let Some((mut world, mut logic)) = server(Some(&root)) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    register_player(&mut cx);
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    for (index, (name, _)) in CHAPTERS.iter().enumerate() {
        for state in STATES {
            let (_, text) = saved(&mut cx, index, state);
            files.push((format!("save_{name}_{state}.ini"), text));
        }
    }
    let story = new_story(&mut cx);
    files.push(("story_constructed.ini".to_owned(), story_save(&cx, &story)));
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "mission saves: {} files, {} bytes",
        files.len(),
        files.iter().map(|(_, t)| t.len()).sum::<usize>()
    );
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let dir = oracle.join("mission_saves");
    let mut problems = Vec::new();
    for (name, text) in &files {
        let path = dir.join(name);
        match std::fs::read(&path) {
            Ok(bytes) => {
                let _ = writeln!(summary, "  {name}: {} bytes", bytes.len());
                problems.extend(first_difference(name, &bytes, text));
            }
            Err(e) => problems.push(format!("{}: {e}", path.display())),
        }
    }
    let _ = writeln!(
        summary,
        "{} files compared with the C++ writer, {} differences",
        files.len(),
        problems.len()
    );
    for p in problems.iter().take(40) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-mission-saves.txt"), &summary);
    assert!(
        problems.is_empty(),
        "mission saves: {} differences from the C++ writer",
        problems.len()
    );
}

/// Every save the port writes for `OracleLoadMissionSaves`, by file name;
/// the text-mode translation is the caller's.
fn rust_saves(cx: &mut Cx<'_>) -> Vec<(String, Vec<u8>)> {
    let raw = text_mode_read(&fixture("raw.ini"));
    let mut files = Vec::new();
    for (index, (name, _)) in CHAPTERS.iter().enumerate() {
        for state in STATES {
            let (_, text) = saved(cx, index, state);
            files.push((format!("save_{name}_{state}.ini"), text));
        }
        let clamps = cx.logic.content.state.clamps;
        let (mut s, _) = restored(cx, index, &raw);
        let text = write_save(cx, &mut s);
        cx.logic.content.state.clamps = clamps;
        files.push((format!("save_{name}_restored.ini"), text));
        files.push((format!("save_{name}_raw.ini"), raw.clone()));
    }
    let mut story = new_story(cx);
    for n in 0..STORIES {
        let text = text_mode_read(&fixture(&format!("story_{n}.ini")));
        let mut reader = reader_over(&text);
        let _ = story::load(cx, &mut story, &mut reader);
        files.push((format!("story_{n}.ini"), story_save(cx, &story)));
    }
    files
}

/// Writes the port's saves into `$OUTPOST_SAVE_LOADS_DIR/rust/` as the
/// server's text-mode save path writes them.
#[test]
fn rust_saves_for_the_oracle() {
    let (Some(root), Some(dir)) = (
        data_root(),
        std::env::var_os("OUTPOST_SAVE_LOADS_DIR").map(PathBuf::from),
    ) else {
        println!(
            "notice: OUTPOST_DATA_DIR or OUTPOST_SAVE_LOADS_DIR is not set; no saves were written for the oracle"
        );
        return;
    };
    let Some((mut world, mut logic)) = server(Some(&root)) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    register_player(&mut cx);
    let out = dir.join("rust");
    std::fs::create_dir_all(&out).expect("the saves' folder");
    let files = rust_saves(&mut cx);
    for (name, text) in &files {
        std::fs::write(out.join(name), text_mode_write(text)).expect("a save");
    }
    println!("{} saves written to {}", files.len(), out.display());
}

/// The C++ restores of the port's saves against the port's.
#[test]
fn oracle_loads_of_rust_saves() {
    let (Some(root), Some(oracle)) = (
        data_root(),
        std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from),
    ) else {
        println!(
            "notice: OUTPOST_DATA_DIR or OUTPOST_ORACLE_DIR is not set; no loads were compared"
        );
        return;
    };
    let rust = oracle.join("rust");
    if !rust.is_dir() {
        println!(
            "notice: {} holds no port saves; no loads were compared",
            rust.display()
        );
        return;
    }
    let Some((mut world, mut logic)) = server(Some(&root)) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    register_player(&mut cx);
    let mut rendered: Vec<(String, Vec<u8>)> = Vec::new();
    let mut problems = Vec::new();
    for (index, (name, _)) in CHAPTERS.iter().enumerate() {
        for state in LOAD_STATES {
            let file = rust.join(format!("save_{name}_{state}.ini"));
            let Ok(bytes) = std::fs::read(&file) else {
                problems.push(format!("{}: missing", file.display()));
                continue;
            };
            let clamps = cx.logic.content.state.clamps;
            let (mut s, n) = restored(&mut cx, index, &bytes);
            rendered.push((
                format!("loaded_{name}_{state}.txt"),
                render_loaded(&cx, &mut s, n),
            ));
            cx.logic.content.state.clamps = clamps;
        }
    }
    let mut story = new_story(&mut cx);
    for n in 0..STORIES {
        let file = rust.join(format!("story_{n}.ini"));
        let Ok(bytes) = std::fs::read(&file) else {
            problems.push(format!("{}: missing", file.display()));
            continue;
        };
        let mut reader = reader_over(&bytes);
        let _ = story::player_load(&mut cx, &mut story, PLAYER, &mut reader);
        let mut text = story_save(&cx, &story);
        let number = cx
            .logic
            .content
            .state
            .players
            .get(&PLAYER)
            .map_or(u32::MAX, |s| s.mission_number);
        text.extend_from_slice(format!("mission_number={number}\n").as_bytes());
        rendered.push((format!("story_loaded_{n}.txt"), text));
    }
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "mission save loads: {} restores of the port's saves",
        rendered.len()
    );
    for (name, text) in &rendered {
        let path = oracle.join(name);
        match std::fs::read(&path) {
            Ok(bytes) => {
                let _ = writeln!(summary, "  {name}: {} bytes", bytes.len());
                problems.extend(first_difference(name, &bytes, text));
            }
            Err(e) => problems.push(format!("{}: {e}", path.display())),
        }
    }
    let _ = writeln!(
        summary,
        "{} restores compared with the C++ restores, {} differences",
        rendered.len(),
        problems.len()
    );
    for p in problems.iter().take(40) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-mission-save-loads.txt"), &summary);
    assert!(
        problems.is_empty(),
        "mission save loads: {} differences from the C++ restores",
        problems.len()
    );
}

/// What a restore changes, as the C++ does, in the state a save was made
/// from: the type 5 and 6 objectives' positions read back reversed, a
/// nickname of 0 read back as `CreateID("0")`, the NPC system exits'
/// systems through a float, the finished cut scenes dropped, an enabled
/// encounter's ship count cleared, and the active triggers (or the pending
/// trigger save) become the initially active triggers, which the run time
/// activates again. The story cue is written but not read: the restored
/// mission keeps its constructor's 0. (The NN path's restore hides the
/// objectives the mission had, but the saved ones replace them after it.)
fn restore_effects(s: &mut MissionScript) {
    s.story_cue = 0;
    for player in s.object.players.values_mut() {
        for o in &mut player.objectives {
            if o.kind == 5 || o.kind == 6 {
                o.marker.swap(2, 4);
            }
            if o.nickname == 0 {
                o.nickname = create_id(b"0");
            }
        }
    }
    for id in &s.active_triggers {
        if let Some(Condition::NpcSystemExit(c)) =
            s.triggers.get_mut(id).and_then(|t| t.condition.as_mut())
        {
            for entry in &mut c.pending {
                entry.system_id = trunc_to_i64(f64::from(entry.system_id as f32)) as u32;
            }
        }
    }
    s.rtc_scripts.retain(|r| !r.finished);
    for e in &mut s.object.rand_encs {
        if e.enabled {
            e.npc_count = 0;
        }
    }
    if s.object.trigger_save_pending {
        s.object.trigger_save_pending = false;
        let id = s.save_trigger_id;
        if s.triggers.get(&id).is_some_and(|t| t.condition.is_some()) {
            s.active_triggers = vec![id];
        }
    }
}

/// Gate G3's save round trip: each mission in each state written, restored
/// into a new mission and written again equals the first save with the
/// restore's effects applied ([`restore_effects`]); the restored triggers
/// are activated in id order first, as the original ones were.
#[test]
fn save_round_trips_on_outdir() {
    let Some((mut world, mut logic)) = server(None) else {
        println!("outdir/ is missing: no mission saves");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    register_player(&mut cx);
    let mut problems = Vec::new();
    let mut count = 0;
    for (index, (name, _)) in CHAPTERS.iter().enumerate() {
        for state in STATES {
            let start = cx.logic.content.state.clamps;
            let mut s = make(&mut cx, index);
            match state {
                "advanced" => advance(&mut cx, &mut s),
                "pending" => pending(&mut s),
                _ => {}
            }
            let first = write_save(&cx, &mut s.clone());
            cx.logic.content.state.clamps = start;
            let (mut back, _) = restored(&mut cx, index, &first);
            if state != "constructed" {
                back.active_triggers = back
                    .initially_active
                    .keys()
                    .copied()
                    .filter(|id| back.triggers.get(id).is_some_and(|t| t.condition.is_some()))
                    .collect();
            }
            let again = write_save(&cx, &mut back);
            let mut expected = s.clone();
            restore_effects(&mut expected);
            let want = write_save(&cx, &mut expected);
            cx.logic.content.state.clamps = start;
            problems.extend(first_difference(&format!("{name} {state}"), &want, &again));
            count += 1;
        }
    }
    println!(
        "{count} round trips, {} differences: {problems:#?}",
        problems.len()
    );
    assert!(problems.is_empty());
}

/// The hand-made fixture restored into Mission_02: the values the readers
/// take from each section, the guide of every ship after a
/// `[MetaBehavior]`, the lists up to `END`, the unnamed vibe parties and
/// the `RandomMission` encounter with its parameters.
#[test]
fn the_raw_fixture_restores_on_outdir() {
    let Some((mut world, mut logic)) = server(None) else {
        println!("outdir/ is missing: no mission saves");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    register_player(&mut cx);
    let raw = text_mode_read(&fixture("raw.ini"));
    let (s, draws) = restored(&mut cx, 2, &raw);
    // [MissionState].
    assert!(s.field_0x2ec);
    let c = cx.logic.content.state.clamps;
    assert_eq!(
        (
            c.attack_clamp,
            c.encounter_enabled,
            c.scan_clamp,
            c.gcs_clamp_enabled(),
            c.hostile_clamp,
            c.population_enabled
        ),
        (1, 0, 1, true, 0, 0)
    );
    assert_eq!(s.object.title, 1234);
    assert_eq!(s.rtc_scripts.len(), 2);
    assert!(s.rtc_scripts[0].field_0x95 && !s.rtc_scripts[1].field_0x95);
    assert_eq!(s.ambient_scenes[0].room_id, 4660);
    assert_eq!(s.ambient_scenes[0].base_id, 0);
    assert_eq!(s.friendly_fire_excused.len(), 2);
    assert_eq!(s.story_cue, 0, "story_cue is written, not read");
    assert!(s.nn_path.active);
    assert_eq!(s.nn_path.target_ids, -5);
    assert_eq!(s.nn_path.position, Vector::new(1.5, 2.5, 3.5));
    assert!(s.nn_path.label.is_empty());
    // [TriggerSave].
    let ids: Vec<u32> = s.initially_active.keys().copied().collect();
    assert_eq!(ids, [12345, u32::MAX]);
    // [NNObjective] up to [BStoryMissionDone].
    let objectives = &s.object.players[&PLAYER].objectives;
    assert_eq!(objectives.len(), 4);
    assert!(objectives[0].hidden && objectives[0].kind == 2);
    assert_eq!(objectives[1].nickname, create_id(b"oracle_objective"));
    assert_eq!(f32::from_bits(objectives[1].marker[2]), 3.75);
    assert_eq!(objectives[2].nickname, u32::MAX);
    assert_eq!(objectives[3].label, b"oracle_label");
    // The respawn records.
    let wings = &s.object.respawn_ships;
    assert_eq!(wings.len(), 2);
    assert_eq!(wings[0].ships.len(), 1);
    let a = &wings[0].ships[0];
    assert_eq!(a.directives.len(), 1);
    assert!(a.guide.is_none());
    assert_eq!(a.init_objectives, local_objective());
    assert_eq!(a.label_ids, [111, 4_000_000_007]);
    assert!(a.jumper && !a.original_jump_flag);
    assert_eq!(wings[1].ships.len(), 2);
    assert!(
        wings[1]
            .ships
            .iter()
            .all(|s| s.guide.is_some() && s.directives.is_empty())
    );
    assert_eq!(wings[1].arrival_obj_id, 1_073_742_115);
    let r = &s.object.rand_enc_respawns[0];
    assert_eq!(r.directives.len(), 1);
    assert_eq!(r.choices[0].ship_classes, [17, 34]);
    assert_eq!(r.choices[1].ship_classes, [5, 0]);
    assert_eq!(r.names.len(), 2 + 0x20);
    // The lists.
    assert_eq!(s.object.respawn_solars, [b"oracle_solar"]);
    assert_eq!(s.object.invulnerables.len(), 2);
    assert!(s.object.invulnerables[0].param3);
    assert_eq!(s.object.decloaked, [b"oracle_cloak"]);
    assert_eq!(s.object.solar_reps, [17, 33, 18]);
    // [RandomEncounter] RandomMission.
    let random = s
        .object
        .rand_encs
        .iter()
        .find(|e| e.nickname == b"RandomMission")
        .expect("the RandomMission encounter");
    let params = random.params.as_ref().expect("its parameters");
    assert_eq!((params.attacker_rep, params.pilot_id), (5, 77));
    assert_eq!(params.formations.len(), 1);
    assert!(draws > 0, "the time activation and num_forms draw");
}

/// Story's save and restore need no data: a new Story, then each
/// hand-made `[StoryInfo]` in turn.
#[test]
fn story_saves_and_restores() {
    let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let state = PlayerState::new(&cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let mut story = new_story(&mut cx);
    assert_eq!(
        story_save(&cx, &story),
        b"[StoryInfo]\nship_bought = false\ndebug = 0\n"
    );
    let mut texts = BTreeMap::new();
    for n in 0..STORIES {
        let text = text_mode_read(&fixture(&format!("story_{n}.ini")));
        let mut reader = reader_over(&text);
        let steps = story::load(&mut cx, &mut story, &mut reader);
        texts.insert(n, String::from_utf8(story_save(&cx, &story)).unwrap());
        if n == 0 {
            assert_eq!(steps, [StoryLoadStep::LoadMission]);
        }
        // A round trip of the save gives the same save.
        let saved = story_save(&cx, &story);
        let mut again = Story::new(PLAYER);
        let mut reader = reader_over(&saved);
        let _ = story::load(&mut cx, &mut again, &mut reader);
        assert_eq!(story_save(&cx, &again), saved, "story {n}");
    }
    assert_eq!(
        texts[&0],
        "[StoryInfo]\nship_bought = true\nMission = No_Mission\nMissionNum = 5\n\
         delta_worth = 3500.500000\ndebug = 1\n"
    );
    assert_eq!(
        texts[&2],
        "[StoryInfo]\nship_bought = true\nMission = no_mission\nMissionNum = 12\n\
         delta_worth = 16777218.000000\ndebug = 0\n"
    );
    assert_eq!(cx.logic.content.state.players[&PLAYER].mission_number, 42);

    // A GenericMission's path and a story mission's creation.
    let mut reader = reader_over(
        b"[StoryInfo]\nMission = GenericMission, missions\\generic\\a.ini\nMissionNum = 9\n\
          Mission = Mission_02\n",
    );
    let steps = story::load(&mut cx, &mut story, &mut reader);
    assert_eq!(
        steps,
        [
            StoryLoadStep::CreateMission {
                name: b"GenericMission".to_vec(),
                path: Some(mission::MissionPath::new(b"missions\\generic\\a.ini")),
            },
            StoryLoadStep::LoadMission,
            StoryLoadStep::CreateMission {
                name: b"Mission_02".to_vec(),
                path: None,
            },
        ]
    );
    // Another player's PlayerSave and PlayerLoad do nothing.
    let mut out = Vec::new();
    story::player_save(&cx, &story, PLAYER + 1, &mut out).unwrap();
    assert!(out.is_empty());
}
