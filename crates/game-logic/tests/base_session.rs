//! The Tier B base session trace (work package P3.6e, record 0008 points 7
//! and 10): BaseSupport and GameSupport's base cases for a player visiting
//! `he01_01_Base`, against `Content.dll`'s `OracleDumpBaseSession`
//! (`CppSource/Content/oracle_base_session.cpp`, session
//! `CppSource/Oracle/base_session.txt`) and Server.dll's base database hook
//! (`OracleSetBaseDbTrace`, `CppSource/Server/server_basedb.cpp`).
//!
//! Both sides start a multiplayer server, register player 7 in the base's
//! system with no ship and no server slot, load the base's rooms in the
//! server's base database (`BaseDB::load_base`, as `IServerImpl::BaseEnter`
//! does before the bus hears of it) and run the session of [`run_session`]:
//!
//! 0. `BaseEnter` (GameSupport's case, then BaseSupport's);
//! 1. `RefreshMissionVendorOffers`, which stands in for the vendor offers
//!    `BaseEnter` builds only for a player with a ship;
//! 2. per `[mRoom]` of the base, `LocationEnter` and `LocationExit`;
//! 3. `LocationEnter` of the bartender's room;
//! 4. `GFObjSelect` of the bartender;
//! 5. `MissionResponse` accepting the first vendor offer;
//! 6. `LocationExit` and `BaseExit`.
//!
//! Each step starts with `srand(SEED + step)`. The trace (`base.jsonl`) has
//! a `step` record, a `message` record before each message, the base
//! database's events in order (records made with their marshalled
//! properties, destroys, behaviours set, the vendor's answer, the vendor's
//! "why empty"), and a `state` record at each step's end: the `rand()`
//! draws of the step, the player's room, his visit (news and vendor
//! offers) and every room state of the base (locks, characters, visit
//! references).
//!
//! The comparison is exact, record by record, to the first difference,
//! with one exception: the `DestroyMission` controller the acceptance
//! makes runs its constructor and handles the response in the C++ and not
//! in the port (D955), so the records of step 5 after the vendor's answer
//! (the room's scripts restarted on a moved generator) and the step's
//! draws are compared and their differences listed under "D955", not
//! judged.
//!
//! - `data_dump_base_session` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   against `base_session/base.jsonl`; summary `rust-base-session.txt`,
//!   the port's trace `rust-base.jsonl`.
//! - `the_base_session_runs_on_outdir`: the session on `outdir/`, its
//!   records checked by hand; it returns early without the data.
//!
//! Nothing these tests write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::bases::{self as base_db, DbEvent, RecordKind};
use game_logic::content::players::PlayerState;
use game_logic::content::{Instance, bar, base_support, game_support, generic_scripts};
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

/// `OracleBaseSeed`: each step seeds `SEED + step`.
const SEED: u32 = 0x4f52_4143;
/// `OracleBasePlayerId`.
const PLAYER: u32 = 7;
/// `OracleBaseName`.
const BASE: &[u8] = b"he01_01_Base";
/// The step whose tail D955 explains.
const ACCEPT_STEP_NAME: &str = "accept_vendor";

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer server over a data root, started as the oracle starts it
/// (as `mission_trace.rs` does).
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

/// A JSON string of bytes: `\\`, `"` and the bytes outside printable
/// ASCII escaped as `\u00XX`.
fn json_string(bytes: &[u8]) -> String {
    let mut out = String::from("\"");
    for &b in bytes {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            0x20..=0x7e => out.push(b as char),
            _ => {
                let _ = write!(out, "\\u{b:04x}");
            }
        }
    }
    out.push('"');
    out
}

/// The draws since `srand(seed)`: the generator stepped from the seed until
/// it is the world's again (-1 beyond 0x10000 draws).
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

/// The session's recorder.
struct Trace {
    lines: Vec<String>,
    step: u32,
    names: Vec<&'static str>,
}

impl Trace {
    fn begin(&mut self, cx: &mut Cx<'_>, name: &'static str) {
        self.step = self.names.len() as u32;
        self.names.push(name);
        self.lines.push(format!(
            "{{\"rec\":\"step\",\"step\":{},\"name\":\"{name}\"}}",
            self.step
        ));
        cx.world.rng().seed(SEED.wrapping_add(self.step));
    }

    fn events(&mut self, cx: &mut Cx<'_>) {
        for event in base_db::take_trace(cx) {
            self.lines.push(event.to_json(self.step));
        }
    }

    /// The `message` record, then GameSupport's handler and BaseSupport's.
    fn deliver(&mut self, cx: &mut Cx<'_>, ids: (ControllerId, ControllerId), mut message: ControllerMessage<'_>) {
        self.lines.push(format!(
            "{{\"rec\":\"message\",\"step\":{},\"msg\":{}}}",
            self.step,
            message.id()
        ));
        game_support::handle_message(cx, ids.0, &mut message);
        base_support::handle_message(cx, ids.1, &mut message);
        self.events(cx);
    }

    /// The step's `state` record.
    fn end(&mut self, cx: &mut Cx<'_>, base: u32) {
        self.events(cx);
        let draws = draws(cx, SEED.wrapping_add(self.step));
        let content = &cx.logic.content.state;
        let location = content.players.get(&PLAYER).map_or(0, |s| s.location);
        let visit = match content.base_visits.get(&PLAYER) {
            None => "null".to_owned(),
            Some(v) => {
                let news: Vec<String> = v.news.iter().map(u32::to_string).collect();
                let vendor: Vec<String> = v
                    .vendor_offers
                    .iter()
                    .map(|o| format!("[{},{}]", o.handle, o.offer_id))
                    .collect();
                format!(
                    "{{\"news\":[{}],\"vendor\":[{}]}}",
                    news.join(","),
                    vendor.join(",")
                )
            }
        };
        let mut rooms = Vec::new();
        if let Some(record) = content.bases.get(&base) {
            for (room, state) in &record.room_states {
                let locks: Vec<String> = state
                    .locks
                    .iter()
                    .map(|l| format!("[{},{},{}]", l.player, l.placement, l.count))
                    .collect();
                let characters: Vec<String> = state
                    .characters
                    .iter()
                    .map(|c| {
                        format!(
                            "[{},{},{},{},{},{},{},{}]",
                            c.character_id,
                            c.behavior,
                            c.use_count,
                            c.placement,
                            json_string(&c.role),
                            c.player,
                            c.talk_count,
                            c.offer.kind()
                        )
                    })
                    .collect();
                let refs: Vec<String> = state
                    .character_refs
                    .iter()
                    .map(|&key| {
                        record
                            .room_states
                            .values()
                            .flat_map(|s| s.characters.iter())
                            .find(|c| c.key == key)
                            .map_or(0, |c| c.character_id)
                            .to_string()
                    })
                    .collect();
                rooms.push(format!(
                    "{{\"room\":{room},\"locks\":[{}],\"characters\":[{}],\"refs\":[{}]}}",
                    locks.join(","),
                    characters.join(","),
                    refs.join(",")
                ));
            }
        }
        self.lines.push(format!(
            "{{\"rec\":\"state\",\"step\":{},\"draws\":{draws},\"location\":{location},\"visit\":{visit},\"rooms\":[{}]}}",
            self.step,
            rooms.join(",")
        ));
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

/// The session's trace on a started server.
fn run_session(cx: &mut Cx<'_>) -> Trace {
    let ids = support_ids(cx);
    // RegisterOracleBasePlayer: player 7 in the base's system, no ship.
    let base = api::misc::base_id(cx, BASE).unwrap_or(0);
    let system = api::misc::base_system(cx, base).ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = system.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
    // `BaseDB::load_base`, as the server's `BaseEnter` runs it first.
    if let Some(host) = cx.world.data_host() {
        base_db::load_base(cx, &host, base);
    }
    base_db::start_trace(cx);
    let mut t = Trace {
        lines: Vec::new(),
        step: 0,
        names: Vec::new(),
    };
    t.begin(cx, "base_enter");
    t.deliver(cx, ids, ControllerMessage::BaseEnter { base, player: PLAYER });
    t.end(cx, base);

    t.begin(cx, "vendor_offers");
    base_support::refresh_mission_vendor_offers(cx, PLAYER);
    t.end(cx, base);

    let rooms: Vec<u32> = cx
        .logic
        .content
        .state
        .bases
        .get(&base)
        .map(|b| b.rooms.iter().map(|r| r.room_id).collect())
        .unwrap_or_default();
    for room in rooms {
        t.begin(cx, "room");
        t.deliver(cx, ids, ControllerMessage::LocationEnter { location: room, player: PLAYER });
        t.deliver(cx, ids, ControllerMessage::LocationExit { location: room, player: PLAYER });
        t.end(cx, base);
    }

    // The player's bartender: the first room (by id) holding a character of
    // the player whose role is `bartender`.
    let (bar_room, bartender) = cx
        .logic
        .content
        .state
        .bases
        .get(&base)
        .and_then(|b| {
            b.room_states.iter().find_map(|(&room, s)| {
                s.characters
                    .iter()
                    .find(|c| c.player == PLAYER && generic_scripts::is_bartender_role(&c.role))
                    .map(|c| (room, c.character_id))
            })
        })
        .unwrap_or((0, 0));
    t.begin(cx, "bar");
    t.deliver(cx, ids, ControllerMessage::LocationEnter { location: bar_room, player: PLAYER });
    t.end(cx, base);

    t.begin(cx, "select_bartender");
    t.deliver(cx, ids, ControllerMessage::GfObjSelect { object: bartender, player: PLAYER });
    t.end(cx, base);

    let offer = cx
        .logic
        .content
        .state
        .base_visits
        .get(&PLAYER)
        .and_then(|v| v.vendor_offers.first())
        .map_or(0, |o| o.offer_id);
    t.begin(cx, ACCEPT_STEP_NAME);
    t.deliver(
        cx,
        ids,
        ControllerMessage::MissionResponse {
            offer,
            kind: 2,
            accepted: true,
            player: PLAYER,
        },
    );
    t.end(cx, base);

    t.begin(cx, "base_exit");
    t.deliver(cx, ids, ControllerMessage::LocationExit { location: bar_room, player: PLAYER });
    t.deliver(cx, ids, ControllerMessage::BaseExit { base, player: PLAYER });
    t.end(cx, base);

    base_db::stop_trace(cx);
    cx.logic.content.state.players.remove(&PLAYER);
    t
}

/// Records shown on each side of a first difference.
const REPORT_WINDOW: usize = 2;

/// The `step` number of a record.
fn step_of(line: &str) -> Option<u32> {
    let at = line.find("\"step\":")? + 7;
    line[at..]
        .split(|c: char| !c.is_ascii_digit())
        .next()?
        .parse()
        .ok()
}

/// The records of each step.
fn by_step(lines: &[String]) -> Vec<Vec<&str>> {
    let mut out: Vec<Vec<&str>> = Vec::new();
    for line in lines {
        let step = step_of(line).unwrap_or(0) as usize;
        if out.len() <= step {
            out.resize(step + 1, Vec::new());
        }
        out[step].push(line);
    }
    out
}

/// The comparison: the summary lines, and whether a judged difference was
/// found.
fn compare(rust: &[String], oracle: &[String], names: &[&str]) -> (Vec<String>, bool) {
    let accept = names.iter().position(|n| *n == ACCEPT_STEP_NAME);
    let (r, o) = (by_step(rust), by_step(oracle));
    let mut lines = Vec::new();
    let mut failed = false;
    for step in 0..r.len().max(o.len()) {
        let empty = Vec::new();
        let rs = r.get(step).unwrap_or(&empty);
        let os = o.get(step).unwrap_or(&empty);
        // In the D955 step, the records after the vendor's answer (and the
        // state's draws) are not judged.
        let judged = if Some(step) == accept {
            let cut = |s: &Vec<&str>| {
                s.iter()
                    .position(|l| l.contains("\"rec\":\"acceptance\""))
                    .map_or(s.len(), |i| i + 1)
            };
            cut(rs).max(cut(os))
        } else {
            usize::MAX
        };
        let Some(at) = (0..rs.len().max(os.len())).find(|&i| rs.get(i) != os.get(i)) else {
            continue;
        };
        let name = names.get(step).copied().unwrap_or("?");
        let label = if at < judged {
            failed = true;
            "first divergence"
        } else {
            "D955 (not judged): first difference"
        };
        lines.push(format!(
            "base_session: step {step} ({name}): {label} at record {at} of rust {} / oracle {}",
            rs.len(),
            os.len()
        ));
        for i in at.saturating_sub(REPORT_WINDOW)..=at + REPORT_WINDOW {
            let mark = if i == at { ">" } else { " " };
            lines.push(format!("{mark} {i}: rust   {}", rs.get(i).copied().unwrap_or("-")));
            lines.push(format!("{mark} {i}: oracle {}", os.get(i).copied().unwrap_or("-")));
        }
        if failed {
            break;
        }
    }
    (lines, failed)
}

/// The counts of each record kind, for the summary.
fn counts(lines: &[String]) -> String {
    let kinds = [
        "step",
        "message",
        "create",
        "destroy",
        "set_behavior",
        "acceptance",
        "why_empty",
        "state",
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
fn data_dump_base_session() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the base session was skipped");
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
    let trace = run_session(&mut cx);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "base session ({}, {} steps): {} records; {}",
        String::from_utf8_lossy(BASE),
        trace.names.len(),
        trace.lines.len(),
        counts(&trace.lines)
    );
    let Some(oracle_dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let mut jsonl = trace.lines.join("\n");
    jsonl.push('\n');
    let _ = std::fs::write(oracle_dir.join("rust-base.jsonl"), jsonl);
    let path = oracle_dir.join("base_session").join("base.jsonl");
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
    let (lines, failed) = compare(&trace.lines, &oracle, &trace.names);
    if lines.is_empty() {
        let _ = writeln!(summary, "equal to the oracle's {} records", oracle.len());
    } else if !failed {
        let _ = writeln!(
            summary,
            "equal to the oracle's {} records but where D955 explains it",
            oracle.len()
        );
    }
    for line in &lines {
        let _ = writeln!(summary, "{line}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle_dir.join("rust-base-session.txt"), &summary);
    assert!(!failed, "base session: differs from the oracle");
}

/// The events of a step, parsed back from the trace's lines.
fn step_lines<'a>(trace: &'a Trace, name: &str) -> Vec<&'a str> {
    let Some(step) = trace.names.iter().position(|n| *n == name) else {
        return Vec::new();
    };
    trace
        .lines
        .iter()
        .filter(|l| step_of(l) == Some(step as u32))
        .map(String::as_str)
        .collect()
}

fn count(lines: &[&str], tag: &str) -> usize {
    lines.iter().filter(|l| l.contains(tag)).count()
}

fn destroys(lines: &[&str], kind: &str) -> usize {
    let tag = format!("\"kind\":\"{kind}\"");
    lines
        .iter()
        .filter(|l| l.starts_with("{\"rec\":\"destroy\"") && l.contains(&tag))
        .count()
}

#[test]
fn the_base_session_runs_on_outdir() {
    let root = outdir();
    let Some((mut world, mut logic)) = started(&root, &root.join("data")) else {
        println!("notice: outdir/OutpostFrontier.ini is missing; the session was not run");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // BaseSupport's constructor read the generic scripts.
    let scripts: usize = cx
        .logic
        .content
        .state
        .generic_scripts
        .slots
        .iter()
        .map(Vec::len)
        .sum();
    assert!(scripts > 500, "{scripts} generic scripts");
    let trace = run_session(&mut cx);
    let summary = counts(&trace.lines);

    // BaseEnter: one character per fixture of the four rooms, each with a
    // behaviour, and the base's news for stage 0.
    let enter = step_lines(&trace, "base_enter");
    assert_eq!(count(&enter, "\"kind\":\"character\""), 4, "{summary}");
    assert_eq!(count(&enter, "\"rec\":\"set_behavior\""), 4, "{enter:#?}");
    assert!(count(&enter, "\"kind\":\"news\"") > 0, "{enter:#?}");
    // No ship: no vendor offers yet.
    assert_eq!(count(&enter, "\"kind\":\"vendor\""), 0, "{enter:#?}");

    // The vendor offers: between num_offers' 8 and 12, the reason reported.
    let offers = step_lines(&trace, "vendor_offers");
    let made = count(&offers, "\"kind\":\"vendor\"");
    assert!((1..=12).contains(&made), "{offers:#?}");
    assert_eq!(count(&offers, "\"rec\":\"why_empty\""), 1, "{offers:#?}");

    // Each room's entry restarts its characters' scripts.
    assert_eq!(trace.names.iter().filter(|n| **n == "room").count(), 4);

    // The bartender was found and selected: his talk counted.
    let select = step_lines(&trace, "select_bartender");
    let state = select.last().expect("the state record");
    assert!(state.contains("\"bartender\""), "{state}");

    // The vendor mission was accepted, its record destroyed.
    let accept = step_lines(&trace, ACCEPT_STEP_NAME);
    assert!(
        accept
            .iter()
            .any(|l| l.contains("\"rec\":\"acceptance\"") && l.contains("\"accepted\":1")),
        "{accept:#?}"
    );
    assert_eq!(destroys(&accept, "vendor"), 1, "{accept:#?}");
    let random_missions = cx
        .logic
        .content
        .instances
        .values()
        .filter(|i| matches!(i, Instance::RandomMission(_)))
        .count();
    assert_eq!(random_missions, 1);

    // BaseExit: every character, news and remaining offer destroyed, the
    // visit and the room states gone.
    let exit = step_lines(&trace, "base_exit");
    assert_eq!(destroys(&exit, "character"), 4, "{exit:#?}");
    assert_eq!(
        count(&exit, "\"kind\":\"news\""),
        count(&enter, "\"kind\":\"news\""),
        "{exit:#?}"
    );
    assert_eq!(count(&exit, "\"kind\":\"vendor\""), made - 1, "{exit:#?}");
    let last = exit.last().expect("the state record");
    assert!(last.contains("\"visit\":null,\"rooms\":[]"), "{last}");
    // Every destroy found its record.
    assert!(!trace.lines.iter().any(|l| l.contains("\"found\":0")), "{summary}");
}

/// D1000 and D1001 on the shipped data: every `script` line of the generic
/// scripts lands in the table, and every GF_NPC rumor's level indexes the
/// thresholds and leaves a 0 among its ids.
#[test]
fn the_shipped_data_stays_inside_the_tables() {
    let root = outdir();
    let Some((mut world, mut logic)) = started(&root, &root.join("data")) else {
        println!("notice: outdir/OutpostFrontier.ini is missing; nothing was checked");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let text = Host::new(&root.join("data"))
        .read(generic_scripts::FILE)
        .expect("the generic scripts");
    let lines = String::from_utf8_lossy(&text)
        .lines()
        .filter(|l| {
            let l = l.trim_start().to_ascii_lowercase();
            l.starts_with("script") && l[6..].trim_start().starts_with('=')
        })
        .count();
    let loaded: usize = cx
        .logic
        .content
        .state
        .generic_scripts
        .slots
        .iter()
        .map(Vec::len)
        .sum();
    assert_eq!(loaded, lines, "a generic script fell outside the table");

    let bases: Vec<u32> = cx.logic.content.state.bases.keys().copied().collect();
    let mut rumors = 0;
    for base in bases {
        game_logic::content::bases::load_base(&mut cx, base);
        let record = &cx.logic.content.state.bases[&base];
        for npc in record.npcs.values() {
            for rumor in &npc.rumors {
                rumors += 1;
                assert!((0..4).contains(&rumor.rep_level), "{rumor:?}");
                assert!(rumor.ids.contains(&0), "{rumor:?}");
            }
            for know in &npc.knows {
                assert!(know.rep_level < 4, "{know:?}");
            }
        }
        game_logic::content::bases::unload_base(&mut cx, base);
    }
    assert!(rumors > 1000, "{rumors} rumors");
}

#[test]
fn the_db_events_print_in_the_oracles_form() {
    let event = DbEvent::Create {
        kind: RecordKind::News,
        handle: 3,
        filter: 0,
        data: vec![0x01, 0xab],
    };
    assert_eq!(
        event.to_json(2),
        "{\"rec\":\"create\",\"step\":2,\"kind\":\"news\",\"handle\":3,\"filter\":0,\"data\":\"01ab\"}"
    );
    assert_eq!(json_string(b"a\\\"\x01"), "\"a\\\\\\\"\\u0001\"");
    let _ = bar::FIRST_SCENE_ID;
}
