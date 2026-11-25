//! The Tier B mission session with a ship (work package P3.5c2, decision
//! record 0008 question 9 and its Verification table): `Mission_01a`'s
//! space part run on the bus through game-logic's server, against
//! `Oracle.exe sim CppSource/Oracle/mission_session.txt` (Content.dll's
//! `OracleSimMission*` exports, `CppSource/Content/oracle_mission_session.cpp`).
//!
//! Both sides start a multiplayer server as the oracle starts it (Content's
//! GameSupport, SpaceSupport and BaseSupport on the scenario's seed, then
//! `Server_LoadWorldState` over `InitialWorld.ini`), load the scenario's
//! system (`StarSystem::load`), seed the generator again, spawn the
//! scenario's ship without behaviour, join it to the system for its `player`
//! session (`OracleSimAddPlayer`), then begin the `mission` line's mission:
//! the session's Content player state with the ship as its ship object (as
//! GameSupport's `OnPlayerLaunch` makes it), the mission created as Story
//! creates one (`ABOVE_NORMAL`, `MissionStart` through the bus at tick 0).
//! Each frame then hands the mission the session's scripted messages of the
//! tick ([`STEPS`], targeted), and runs the server's frame
//! (`IServerImpl::Update`, the global time, `ElapseTime`: the systems, the
//! controllers' heartbeats and timers).
//!
//! Three records are compared:
//!
//! - **`mission.jsonl`**, the trigger trace (`game_logic::content::mission::trace`)
//!   with a `message` record before every delivery of the bus to the
//!   mission, and `final`, or `gone` when the bus tore the mission down;
//! - **`dispatch.jsonl`**, the bus's deliveries (record 0006 point 10) from
//!   the first one to the mission on, as `(tick, depth, controller, message)`
//!   and a heartbeat's elapsed time; a controller is named by its place in
//!   the bus's list (the oracle's list without its harness controller,
//!   `session.jsonl`'s `heartbeats`);
//! - **`mission_objects.jsonl`**, the mission's live NPC ships and spawned
//!   solars and loot after each frame, by kind and nickname.
//!
//! **The comparison rule.** The judged window ends at the first action that
//! still runs without an effect (D970) in the port's trace, at tick `J`:
//! the trace is exact up to and including that record, the dispatch order
//! exact over the ticks before `J`, and every object within the scenario
//! envelope's gross bound (`game_sim::scenario::GROSS_POSITION`, 500 m)
//! over the ticks up to `J` (so the spawns of tick `J` are judged). After
//! the window everything is reported, not judged: the first difference of
//! each record with the records around it, as the scenario judge reports a
//! first divergence, each object's bit-exactness or first divergence, and
//! the unported actions that ran.
//!
//! - `data_dump_mission_session` (`OUTPOST_DATA_DIR`, `OUTPOST_ORACLE_DIR`):
//!   against `mission_session/`; summary `rust-mission-session.txt`, the
//!   port's records `rust-mission-session.jsonl` and
//!   `rust-mission-objects.jsonl`.
//! - `the_space_part_runs_on_outdir`: the session on `outdir/`, its spawns
//!   checked by hand; it returns early without the data.
//!
//! Nothing these tests write is committed.

use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::api::ai::{BaseOp, CallbackArg, Op, SetPersonalityParams};
use game_logic::content::Instance;
use game_logic::content::mission::execute;
use game_logic::content::mission::trace::{Trace, TraceRecord};
use game_logic::content::mission::trigger::Trigger;
use game_logic::content::mission::{MissionScript, engine};
use game_logic::content::players::{PlayerShip, PlayerState};
use game_logic::events::{CommResult, ControllerMessage};
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_logic::{selection, world};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, ShipSpawn};
use game_sim::scenario::records::{Json, read_lines};
use game_sim::scenario::{GROSS_POSITION, Scenario, dist, parse, spawn_orientation, world_config};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// One scripted message of the session (`FOracleSessionStep`).
#[derive(Clone, Copy)]
enum Step {
    /// `SpaceEnter {system, player, 0, 0}`.
    SpaceEnter,
    /// `CommComplete {0, system, 0, line, 0}`.
    CommComplete(&'static [u8]),
}

/// `OracleSessionSteps`: by tick, before the tick's frame.
const STEPS: [(u32, Step); 4] = [
    (2, Step::SpaceEnter),
    (200, Step::CommComplete(b"DX_M01_0000_FREEPORT_7_BRIDGE")),
    (260, Step::CommComplete(b"DX_M01_0005_Samura_SA_14")),
    (320, Step::CommComplete(b"DX_M01_0010_FREEPORT_7_BRIDGE")),
];

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn session_path() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../CppSource/Oracle/mission_session.txt")
}

/// The session file: the scenario, its `player <session> <ship>` lines and
/// its `mission <name> <session>` line.
struct SessionFile {
    scenario: Scenario,
    players: Vec<(u32, String)>,
    mission: (String, u32),
}

fn read_session() -> Option<SessionFile> {
    let path = session_path();
    let scenario = parse(&path)?;
    let text = std::fs::read_to_string(&path).ok()?;
    let mut players = Vec::new();
    let mut mission = None;
    for line in text.lines() {
        let t: Vec<&str> = line.split('#').next()?.split_whitespace().collect();
        match t.as_slice() {
            [k, session, ship] if k.eq_ignore_ascii_case("player") => {
                players.push((session.parse().ok()?, (*ship).to_owned()));
            }
            [k, name, session] if k.eq_ignore_ascii_case("mission") => {
                mission = Some(((*name).to_owned(), session.parse().ok()?));
            }
            _ => {}
        }
    }
    Some(SessionFile {
        scenario,
        players,
        mission: mission?,
    })
}

/// One delivery of the bus (`dispatch.jsonl`'s `deliver`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Delivery {
    tick: u32,
    depth: u32,
    flid: u32,
    msg: u32,
    /// A heartbeat's elapsed time, as bits.
    elapsed: Option<u32>,
}

/// One object after a frame (`mission_objects.jsonl`'s `obj`).
#[derive(Clone, Debug, PartialEq)]
struct ObjRecord {
    tick: u32,
    kind: &'static str,
    name: String,
    pos: [u32; 3],
}

thread_local! {
    static TICK: Cell<u32> = const { Cell::new(0) };
    static DEPTH: Cell<u32> = const { Cell::new(0) };
    static MISSION: Cell<u32> = const { Cell::new(0) };
    static DELIVERIES: RefCell<Vec<Delivery>> = const { RefCell::new(Vec::new()) };
}

/// The bus's hook before each handler: the delivery, and the mission's
/// `message` record.
fn before_deliver(
    cx: &mut Cx<'_>,
    id: ControllerId,
    _: ControllerKind,
    message: &mut ControllerMessage<'_>,
) {
    let tick = TICK.with(Cell::get);
    let depth = DEPTH.with(Cell::get);
    let msg = message.id();
    let elapsed = match message {
        ControllerMessage::Heartbeat { elapsed } => Some(elapsed.to_bits()),
        _ => None,
    };
    DELIVERIES.with(|d| {
        d.borrow_mut().push(Delivery {
            tick,
            depth,
            flid: id.0,
            msg,
            elapsed,
        });
    });
    DEPTH.with(|d| d.set(depth + 1));
    if id.0 == MISSION.with(Cell::get)
        && let Some(Instance::Mission(s)) = cx.logic.content.instances.get_mut(&id)
        && let Some(t) = s.trace.as_mut()
    {
        t.records.push(TraceRecord::Message { tick, msg });
    }
}

fn after_deliver(
    _: &mut Cx<'_>,
    _: ControllerId,
    _: ControllerKind,
    _: &mut ControllerMessage<'_>,
) {
    DEPTH.with(|d| d.set(d.get().saturating_sub(1)));
}

/// What the port's session recorded.
struct Session {
    trace: Vec<TraceRecord>,
    deliveries: Vec<Delivery>,
    objects: Vec<ObjRecord>,
    /// The bus's controllers when the mission was made, in list order.
    controllers: Vec<u32>,
    mission: u32,
    /// Each action run without an effect: tick, trigger, place, class.
    unported: Vec<(u32, u32, u32, &'static str)>,
}

fn script_of<'c>(cx: &'c Cx<'_>, id: ControllerId) -> Option<&'c MissionScript> {
    match cx.logic.content.instances.get(&id) {
        Some(Instance::Mission(s)) => Some(s),
        _ => None,
    }
}

/// The mission's objects after a frame: its NPC ships in its map's order,
/// then its spawned solars and loot, each whose location is known.
fn objects_after_frame(cx: &Cx<'_>, id: ControllerId, tick: u32, out: &mut Vec<ObjRecord>) {
    let Some(s) = script_of(cx, id) else { return };
    let mut push = |kind: &'static str, name: &[u8], object: u32| {
        if let Ok((p, _)) = api::space_obj::location(cx, object) {
            out.push(ObjRecord {
                tick,
                kind,
                name: String::from_utf8_lossy(neuron_core::hash::c_str(name)).into_owned(),
                pos: [p.x.to_bits(), p.y.to_bits(), p.z.to_bits()],
            });
        }
    };
    for (&object, key) in &s.object.npc_objects {
        if let Some(n) = cx.logic.content.state.npc_ships.get(key) {
            push("npc", &n.nickname, object);
        }
    }
    for solar in s.object.solars.iter().filter(|o| o.spawned == 1) {
        push("solar", &solar.nickname, solar.space_obj_id);
    }
    for loot in s.object.loot.iter().filter(|l| l.spawned == 1) {
        push("loot", &loot.nickname, loot.space_obj_id);
    }
}

/// The session's player state and ship object (`OracleSimMissionBegin`,
/// GameSupport's `OnPlayerLaunch` with the scenario's archetype).
fn register_player(cx: &mut Cx<'_>, session: u32, ship: u32, archetype: u32, system: u32) {
    let mut state = PlayerState::new(cx, session);
    let record = Some(system).filter(|s| cx.logic.content.state.systems.contains_key(s));
    state.system = record;
    let mut class_ids: Vec<u32> = Vec::new();
    if let Some(properties) = cx
        .logic
        .content
        .tables
        .ship_mission_properties
        .get(&archetype)
    {
        for &p in properties {
            if !class_ids.contains(&p) {
                class_ids.push(p);
            }
        }
    }
    let kind = api::space_obj::object_type(cx, ship).unwrap_or(0);
    let class = if kind & 0x1_0000 != 0 {
        Some(create_id(b"fighter"))
    } else if kind & 0x2_0000 != 0 {
        Some(create_id(b"freighter"))
    } else {
        None
    };
    if let Some(class) = class
        && !class_ids.contains(&class)
    {
        class_ids.push(class);
    }
    let state_graph = cx
        .world
        .data()
        .state_graphs
        .find(b"PLAYER", GraphType::Leader);
    let op = SetPersonalityParams {
        state_graph,
        content_callback: CallbackArg::Null,
        directive_callback: CallbackArg::Null,
        set_personality: false,
        ..SetPersonalityParams::default()
    };
    let _ = api::ai::submit_state(
        cx,
        ship,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(op))),
    );
    state.ship = Some(PlayerShip::new(ship, archetype, class_ids, record));
    cx.logic.content.state.players.insert(session, state);
}

/// The session on a data root (`exe` holds `OutpostFrontier.ini`).
#[allow(clippy::too_many_lines)]
fn run_session(exe: &Path, data_dir: &Path, file: &SessionFile) -> Option<Session> {
    let s = &file.scenario;
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut world = World::new(Arc::new(data), world_config(s))
        .with_assets(Assets::new(data_dir.to_path_buf()));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // `IServerImpl::Startup` on the scenario's seed, the start-up's
    // controllers, then `Server_LoadWorldState`.
    cx.world.rng().seed(s.seed);
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

    // The system's load, the seeding point, the ship and the player.
    cx.world.rng().seed(s.seed);
    let system = create_id(s.system.as_bytes());
    world::load(&mut cx, system);
    cx.world.rng().seed(s.reseed.unwrap_or(s.seed));
    let (session, mission_name) = (file.mission.1, file.mission.0.clone());
    let ship_name = file.players.iter().find(|(p, _)| *p == session)?.1.clone();
    let desc = s.objects.iter().find(|o| o.name == ship_name && o.ship)?;
    let archetype = create_id(desc.archetype.as_bytes());
    let mut spawn = ShipSpawn::new(
        archetype,
        create_id(desc.loadout.as_bytes()),
        desc.pos,
        spawn_orientation(&desc.orient),
    );
    spawn.velocity = desc.vel;
    spawn.angular_velocity = desc.angvel;
    spawn.hit_pts = desc.hp;
    let handle = cx.world.spawn_ship(system, &spawn).ok()?;
    let ship = cx.world.get(handle)?.simple.id;
    let slot = cx.logic.players.get_mut(session)?;
    slot.system_id = system;
    slot.ship_object_id = ship;
    world::add_player(&mut cx, system, session);

    // `OracleSimMissionBegin`.
    register_player(&mut cx, session, ship, archetype, system);
    cx.logic.content.state.mission_dialogs.clear();
    cx.logic.before_deliver_hook = Some(before_deliver);
    cx.logic.deliver_hook = Some(after_deliver);
    TICK.with(|t| t.set(0));
    DEPTH.with(|d| d.set(0));
    DELIVERIES.with(|d| d.borrow_mut().clear());
    MISSION.with(|m| m.set(u32::MAX));
    let params = CreateParams::StoryMission {
        player: session,
        story: ControllerId::BROADCAST,
    };
    let id = controller::create(
        &mut cx,
        b"Content.dll",
        mission_name.as_bytes(),
        &params,
        Priority::AboveNormal,
    );
    if id.0 == 0 || script_of(&cx, id).is_none() {
        return None;
    }
    MISSION.with(|m| m.set(id.0));
    engine::start_trace(&mut cx, id);
    let triggers: BTreeMap<u32, Trigger> = script_of(&cx, id)
        .map(|s| s.triggers.clone())
        .unwrap_or_default();
    let controllers: Vec<u32> = cx.logic.bus.active().iter().map(|n| n.id.0).collect();
    let _ = cx.dispatch(id, &mut ControllerMessage::MissionStart);

    let dt = s.frame as f32;
    let mut objects = Vec::new();
    let mut gone_trace: Option<Trace> = None;
    for tick in 1..=s.ticks {
        TICK.with(|t| t.set(tick));
        if let Some(Instance::Mission(m)) = cx.logic.content.instances.get_mut(&id)
            && let Some(t) = m.trace.as_mut()
        {
            t.tick = tick;
        }
        for &(_, step) in STEPS.iter().filter(|(t, _)| *t == tick) {
            let mut message = match step {
                Step::SpaceEnter => ControllerMessage::SpaceEnter {
                    system,
                    player: session,
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
            };
            let _ = cx.dispatch(id, &mut message);
        }
        // `IServerImpl::Update` (no client), the global time, `ElapseTime`.
        cx.world.update_global_time(dt);
        neuron_server::tick::elapse_time(&mut cx, dt, s.frame);
        objects_after_frame(&cx, id, tick, &mut objects);
        if gone_trace.is_none()
            && let Some(at) = cx
                .logic
                .content
                .finished_traces
                .iter()
                .position(|(c, _)| *c == id)
        {
            gone_trace = Some(cx.logic.content.finished_traces.remove(at).1);
        }
    }
    let mut trace = if let Some(t) = gone_trace {
        let mut t = t;
        t.records.push(TraceRecord::Gone { tick: s.ticks });
        t.records
    } else {
        engine::finish_trace(&mut cx, id);
        script_of(&cx, id)
            .and_then(|m| m.trace.as_ref())
            .map(|t| t.records.clone())
            .unwrap_or_default()
    };
    // A trace whose mission was torn down without being found.
    if trace.is_empty() {
        trace.push(TraceRecord::Gone { tick: s.ticks });
    }
    let unported = trace
        .iter()
        .filter_map(|r| match *r {
            TraceRecord::Action {
                tick,
                trigger,
                index,
            }
            | TraceRecord::Timer {
                tick,
                trigger,
                index,
            } => {
                let action = triggers.get(&trigger)?.actions.get(index as usize)?;
                (!execute::is_ported(action)).then(|| (tick, trigger, index, action.name()))
            }
            _ => None,
        })
        .collect();
    cx.logic.before_deliver_hook = None;
    cx.logic.deliver_hook = None;
    let deliveries = DELIVERIES.with(|d| std::mem::take(&mut *d.borrow_mut()));
    Some(Session {
        trace,
        deliveries,
        objects,
        controllers,
        mission: id.0,
        unported,
    })
}

// ---- the comparison --------------------------------------------------------------------------

/// Records shown on each side of a first difference.
const REPORT_WINDOW: usize = 2;

/// The first difference of two sequences, with the records around it.
fn first_difference(
    what: &str,
    rust: &[String],
    oracle: &[String],
) -> Option<(usize, Vec<String>)> {
    let at = (0..rust.len().max(oracle.len())).find(|&i| rust.get(i) != oracle.get(i))?;
    let mut lines = vec![format!(
        "{what}: first divergence at record {at} of rust {} / oracle {}",
        rust.len(),
        oracle.len()
    )];
    for i in at.saturating_sub(REPORT_WINDOW)..=at + REPORT_WINDOW {
        let r = rust.get(i).map_or("-", String::as_str);
        let o = oracle.get(i).map_or("-", String::as_str);
        let mark = if i == at { ">" } else { " " };
        lines.push(format!("{mark} {i}: rust   {r}"));
        lines.push(format!("{mark} {i}: oracle {o}"));
    }
    Some((at, lines))
}

/// A delivery as the comparison sees it: the controller by its place in
/// the list (`?` for one not listed).
fn delivery_line(d: &Delivery, roles: &BTreeMap<u32, usize>) -> String {
    let role = roles
        .get(&d.flid)
        .map_or_else(|| format!("?{}", d.flid), ToString::to_string);
    match d.elapsed {
        Some(e) => format!(
            "tick {} depth {} controller {role} msg {:#x} elapsed {:08x}",
            d.tick, d.depth, d.msg, e
        ),
        None => format!(
            "tick {} depth {} controller {role} msg {:#x}",
            d.tick, d.depth, d.msg
        ),
    }
}

/// The deliveries from the first one to `mission` on.
fn from_mission(deliveries: &[Delivery], mission: u32) -> &[Delivery] {
    let start = deliveries
        .iter()
        .position(|d| d.flid == mission)
        .unwrap_or(deliveries.len());
    &deliveries[start..]
}

fn oracle_deliveries(path: &Path) -> Vec<Delivery> {
    read_lines(path)
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("deliver"))
        .filter_map(|r| {
            Some(Delivery {
                tick: r.get("tick")?.as_u32()?,
                depth: r.get("depth")?.as_u32()?,
                flid: r.get("flid")?.as_u32()?,
                msg: r.get("msg")?.as_u32()?,
                elapsed: r.get("elapsed").and_then(Json::as_f32).map(f32::to_bits),
            })
        })
        .collect()
}

fn oracle_objects(path: &Path) -> Vec<ObjRecord> {
    read_lines(path)
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("obj"))
        .filter_map(|r| {
            let pos = r.get("pos")?.as_array()?;
            let kind = match r.get("kind")?.as_str()? {
                "npc" => "npc",
                "solar" => "solar",
                "loot" => "loot",
                _ => return None,
            };
            Some(ObjRecord {
                tick: r.get("tick")?.as_u32()?,
                kind,
                name: r.get("name")?.as_str()?.to_owned(),
                pos: [
                    pos.first()?.as_u32()?,
                    pos.get(1)?.as_u32()?,
                    pos.get(2)?.as_u32()?,
                ],
            })
        })
        .collect()
}

/// Each object's records by tick, keyed by kind and nickname (a second
/// object of the same name in a tick gets `#2`, and so on).
fn by_object(records: &[ObjRecord]) -> BTreeMap<String, BTreeMap<u32, [u32; 3]>> {
    let mut out: BTreeMap<String, BTreeMap<u32, [u32; 3]>> = BTreeMap::new();
    let mut seen: BTreeMap<(u32, String), u32> = BTreeMap::new();
    for r in records {
        let base = format!("{} {}", r.kind, r.name);
        let n = seen.entry((r.tick, base.clone())).or_insert(0);
        *n += 1;
        let key = if *n == 1 { base } else { format!("{base}#{n}") };
        out.entry(key).or_default().insert(r.tick, r.pos);
    }
    out
}

fn vector(bits: [u32; 3]) -> Vector {
    Vector::new(
        f32::from_bits(bits[0]),
        f32::from_bits(bits[1]),
        f32::from_bits(bits[2]),
    )
}

/// The objects: judged within the gross bound over the ticks up to
/// `judged`, each object's bit-exactness or first divergence reported.
fn compare_objects(
    summary: &mut String,
    failures: &mut Vec<String>,
    rust: &[ObjRecord],
    oracle: &[ObjRecord],
    judged: u32,
) {
    let ours = by_object(rust);
    let theirs = by_object(oracle);
    for (name, a) in &ours {
        let Some(b) = theirs.get(name) else {
            let first = a.keys().next().copied().unwrap_or(0);
            let _ = writeln!(
                summary,
                "objects: {name}: only in the port's run (from tick {first})"
            );
            if first <= judged {
                failures.push(format!(
                    "{name} exists only in the port's run at tick {first}"
                ));
            }
            continue;
        };
        let mut worst = 0.0_f64;
        let mut first_diff = None;
        for (tick, pa) in a {
            let Some(pb) = b.get(tick) else { continue };
            if pa != pb && first_diff.is_none() {
                first_diff = Some((*tick, *pa, *pb));
            }
            let d = dist(vector(*pa), vector(*pb));
            if *tick <= judged {
                worst = worst.max(d);
                if d.is_nan() || d > GROSS_POSITION {
                    failures.push(format!("{name} at tick {tick}: {d:.3} m from the oracle's"));
                }
            }
        }
        match first_diff {
            None if a.len() == b.len() => {
                let _ = writeln!(summary, "objects: {name}: bit-exact over {} ticks", a.len());
            }
            None => {
                let _ = writeln!(
                    summary,
                    "objects: {name}: the common ticks are bit-exact; ticks rust {} oracle {}",
                    a.len(),
                    b.len()
                );
            }
            Some((tick, pa, pb)) => {
                let _ = writeln!(
                    summary,
                    "objects: {name}: first divergence at tick {tick}: rust {pa:08x?} ({:?}) oracle {pb:08x?} ({:?}); largest distance over the judged ticks {worst:.3} m",
                    vector(pa),
                    vector(pb)
                );
            }
        }
    }
    for (name, b) in &theirs {
        if !ours.contains_key(name) {
            let first = b.keys().next().copied().unwrap_or(0);
            let _ = writeln!(
                summary,
                "objects: {name}: only in the oracle's run (from tick {first})"
            );
            if first <= judged {
                failures.push(format!(
                    "{name} exists only in the oracle's run at tick {first}"
                ));
            }
        }
    }
}

/// The counts of each record kind.
fn counts(records: &[TraceRecord]) -> String {
    let mut n = [0usize; 8];
    for r in records {
        let k = match r {
            TraceRecord::Message { .. } => 0,
            TraceRecord::Activate { .. } => 1,
            TraceRecord::Fire { .. } => 2,
            TraceRecord::Action { .. } => 3,
            TraceRecord::Timer { .. } => 4,
            TraceRecord::Rearm { .. } => 5,
            TraceRecord::Deactivate { .. } => 6,
            TraceRecord::State { .. } | TraceRecord::Gone { .. } | TraceRecord::Final { .. } => 7,
        };
        n[k] += 1;
    }
    format!(
        "messages {} activations {} firings {} actions {} timers {} rearms {} deactivations {} states and ends {}",
        n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]
    )
}

/// The unported actions that ran, by class with their first tick.
fn unported_summary(unported: &[(u32, u32, u32, &'static str)]) -> String {
    let mut first: BTreeMap<&str, (u32, usize)> = BTreeMap::new();
    for &(tick, _, _, name) in unported {
        let e = first.entry(name).or_insert((tick, 0));
        e.1 += 1;
    }
    let mut list: Vec<_> = first.into_iter().collect();
    list.sort_by_key(|(_, (tick, _))| *tick);
    list.iter()
        .map(|(name, (tick, n))| format!("{name} x{n} from tick {tick}"))
        .collect::<Vec<_>>()
        .join(", ")
}

/// The oracle's controllers without its harness's, in list order
/// (`session.jsonl`'s `heartbeats` and `server`).
fn oracle_controllers(path: &Path) -> (Vec<u32>, u32) {
    let records = read_lines(path);
    let harness = records
        .iter()
        .find(|r| r.get("rec").and_then(Json::as_str) == Some("server"))
        .and_then(|r| r.get("controller")?.as_u32())
        .unwrap_or(0);
    let mission = records
        .iter()
        .find(|r| r.get("rec").and_then(Json::as_str) == Some("mission"))
        .and_then(|r| r.get("controller")?.as_u32())
        .unwrap_or(0);
    let list = records
        .iter()
        .find(|r| r.get("rec").and_then(Json::as_str) == Some("heartbeats"))
        .and_then(|r| r.get("controllers")?.as_array().map(<[Json]>::to_vec))
        .unwrap_or_default()
        .iter()
        .filter_map(|c| c.get("flid")?.as_u32())
        .filter(|&f| f != harness)
        .collect();
    (list, mission)
}

#[test]
#[allow(clippy::too_many_lines)]
fn data_dump_mission_session() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the mission session was skipped");
        return;
    };
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let Some(ours) = run_session(&root.join("EXE"), &root.join("DATA"), &file) else {
        println!("notice: the session could not start on the dump root");
        return;
    };
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "mission session ({}, {} ticks): {} trace records ({}); {} deliveries from the mission's first; {} object records",
        file.mission.0,
        file.scenario.ticks,
        ours.trace.len(),
        counts(&ours.trace),
        from_mission(&ours.deliveries, ours.mission).len(),
        ours.objects.len()
    );
    let judged_tick = ours.unported.first().map_or(u32::MAX, |u| u.0);
    let _ = writeln!(
        summary,
        "unported actions that ran (D970): {}",
        if ours.unported.is_empty() {
            "none".to_owned()
        } else {
            unported_summary(&ours.unported)
        }
    );
    let _ = writeln!(
        summary,
        "judged window: to the first unported action, tick {}",
        if judged_tick == u32::MAX {
            "none (the whole session)".to_owned()
        } else {
            judged_tick.to_string()
        }
    );
    let rust_trace: Vec<String> = ours.trace.iter().map(TraceRecord::to_json).collect();
    let Some(oracle_dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let mut jsonl = rust_trace.join("\n");
    jsonl.push('\n');
    let _ = std::fs::write(oracle_dir.join("rust-mission-session.jsonl"), jsonl);
    let objects: String = ours
        .objects
        .iter()
        .map(|o| {
            format!(
                "{{\"rec\":\"obj\",\"tick\":{},\"kind\":\"{}\",\"name\":\"{}\",\"pos\":[{},{},{}]}}\n",
                o.tick, o.kind, o.name, o.pos[0], o.pos[1], o.pos[2]
            )
        })
        .collect();
    let _ = std::fs::write(oracle_dir.join("rust-mission-objects.jsonl"), objects);
    let dir = oracle_dir.join("mission_session");
    let mut failures = Vec::new();

    // The trigger trace.
    let text = match std::fs::read_to_string(dir.join("mission.jsonl")) {
        Ok(text) => text,
        Err(e) => panic!("{}: {e}", dir.join("mission.jsonl").display()),
    };
    let oracle_trace: Vec<String> = text
        .lines()
        .map(str::trim_end)
        .filter(|l| !l.is_empty())
        .map(str::to_owned)
        .collect();
    // The judged records: up to and including the first unported action.
    let judged_records = ours
        .unported
        .first()
        .and_then(|&(tick, trigger, index, _)| {
            ours.trace.iter().position(|r| {
                matches!(*r, TraceRecord::Action { tick: t, trigger: g, index: i } | TraceRecord::Timer { tick: t, trigger: g, index: i } if t == tick && g == trigger && i == index)
            })
        })
        .map_or(usize::MAX, |at| at + 1);
    match first_difference("mission.jsonl", &rust_trace, &oracle_trace) {
        None => {
            let _ = writeln!(
                summary,
                "mission.jsonl: equal to the oracle's {} records",
                oracle_trace.len()
            );
        }
        Some((at, lines)) => {
            for line in lines {
                let _ = writeln!(summary, "{line}");
            }
            if at < judged_records {
                failures.push(format!(
                    "mission.jsonl differs at record {at}, inside the judged window"
                ));
            } else {
                let _ = writeln!(
                    summary,
                    "mission.jsonl: the difference is after the judged window (reported)"
                );
            }
        }
    }

    // The dispatch order.
    let (their_controllers, their_mission) = oracle_controllers(&dir.join("session.jsonl"));
    let their_roles: BTreeMap<u32, usize> = their_controllers
        .iter()
        .enumerate()
        .map(|(i, &f)| (f, i))
        .collect();
    let our_roles: BTreeMap<u32, usize> = ours
        .controllers
        .iter()
        .enumerate()
        .map(|(i, &f)| (f, i))
        .collect();
    let ours_d: Vec<String> = from_mission(&ours.deliveries, ours.mission)
        .iter()
        .map(|d| delivery_line(d, &our_roles))
        .collect();
    let their_all = oracle_deliveries(&dir.join("dispatch.jsonl"));
    let harness: Vec<u32> = read_lines(&dir.join("session.jsonl"))
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("server"))
        .filter_map(|r| r.get("controller")?.as_u32())
        .collect();
    let their_d: Vec<String> = from_mission(&their_all, their_mission)
        .iter()
        .filter(|d| !harness.contains(&d.flid))
        .map(|d| delivery_line(d, &their_roles))
        .collect();
    let _ = writeln!(
        summary,
        "controllers: rust {:?} oracle {:?} (the oracle's harness {:?} left out)",
        ours.controllers, their_controllers, harness
    );
    match first_difference("dispatch.jsonl", &ours_d, &their_d) {
        None => {
            let _ = writeln!(
                summary,
                "dispatch.jsonl: equal to the oracle's {} deliveries",
                their_d.len()
            );
        }
        Some((at, lines)) => {
            for line in lines {
                let _ = writeln!(summary, "{line}");
            }
            let tick = from_mission(&ours.deliveries, ours.mission)
                .get(at)
                .map_or(u32::MAX, |d| d.tick)
                .min(
                    from_mission(&their_all, their_mission)
                        .iter()
                        .filter(|d| !harness.contains(&d.flid))
                        .nth(at)
                        .map_or(u32::MAX, |d| d.tick),
                );
            if tick < judged_tick {
                failures.push(format!("dispatch.jsonl differs at delivery {at} (tick {tick}), inside the judged window"));
            } else {
                let _ = writeln!(
                    summary,
                    "dispatch.jsonl: the difference is at tick {tick}, after the judged window (reported)"
                );
            }
        }
    }

    // The objects.
    let their_objects = oracle_objects(&dir.join("mission_objects.jsonl"));
    compare_objects(
        &mut summary,
        &mut failures,
        &ours.objects,
        &their_objects,
        judged_tick,
    );

    let _ = writeln!(
        summary,
        "judged: {}",
        if failures.is_empty() {
            "no difference".to_owned()
        } else {
            failures.join("; ")
        }
    );
    print!("{summary}");
    let _ = std::fs::write(oracle_dir.join("rust-mission-session.txt"), &summary);
    assert!(failures.is_empty(), "mission session: {failures:?}");
}

#[test]
fn the_space_part_runs_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let Some(ours) = run_session(&root, &root.join("data"), &file) else {
        println!("notice: outdir/ is missing or the session could not start; it was not run");
        return;
    };
    let id = |name: &[u8]| create_id(name);
    let fired = |trigger: u32| {
        ours.trace.iter().find_map(|r| match *r {
            TraceRecord::Fire {
                tick,
                trigger: t,
                event,
            } if t == trigger => Some((tick, event)),
            _ => None,
        })
    };
    // MissionStart is the first delivery to the mission, at tick 0.
    assert_eq!(
        ours.trace.first(),
        Some(&TraceRecord::Message {
            tick: 0,
            msg: 0x1000
        }),
        "{}",
        counts(&ours.trace)
    );
    assert_eq!(fired(id(b"tr_initialize_init")), Some((0, engine::PROBE)));
    // The camera trigger fires on the player's SpaceEnter.
    assert_eq!(fired(id(b"tr_fp7_cam")), Some((2, 0x36)));
    // Its spawns: the three escorts and the transport, Freeport 7 and its
    // domes, all in the mission's objects after the frame.
    let at_two: Vec<&ObjRecord> = ours.objects.iter().filter(|o| o.tick == 2).collect();
    for (kind, name) in [
        ("npc", "fp7base_escort1"),
        ("npc", "fp7base_escort2"),
        ("npc", "fp7base_escort3"),
        ("solar", "fp7base"),
        ("solar", "freeport7_space_dome1"),
    ] {
        assert!(
            at_two
                .iter()
                .any(|o| o.kind == kind && o.name.eq_ignore_ascii_case(name)),
            "{kind} {name} after tick 2: {:?}",
            at_two
                .iter()
                .map(|o| format!("{} {}", o.kind, o.name))
                .collect::<Vec<_>>()
        );
    }
    // The bus delivers the mission its own ships' NpcShipCreated.
    assert!(
        ours.deliveries
            .iter()
            .any(|d| d.flid == ours.mission && d.msg == 0x1010)
    );
    // Heartbeats reach the mission from the bus.
    assert!(
        ours.deliveries
            .iter()
            .any(|d| d.flid == ours.mission && d.msg == 0x20)
    );
    // Act_MovePlayer put the player's ship at its point.
    assert!(
        !ours.unported.is_empty(),
        "M01a runs actions of later increments"
    );
    println!(
        "{} trace records ({}), unported: {}",
        ours.trace.len(),
        counts(&ours.trace),
        unported_summary(&ours.unported)
    );
}
