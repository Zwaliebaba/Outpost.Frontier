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
//! controllers' heartbeats and timers). The harness's player has no client,
//! so the server would step no system; as the oracle's harness does
//! (`GOracleSimHooks.StepSystemId`), the scenario's system is stepped
//! every frame (`ServerWorld::step_system`).
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
//!   checked by hand; it returns early without the data. Since P3.5c4 no
//!   action without an effect runs in its 900 ticks, so the judged window
//!   is the whole session.
//! - `the_ai_and_reputation_actions_on_outdir` (P3.5c3),
//!   `the_objectives_path_nags_and_end_on_outdir` (P3.5c5),
//!   `the_dialogs_speak_and_complete_on_outdir`,
//!   `a_cut_scene_starts_and_ends_on_outdir` and
//!   `a_guided_ship_respawns_with_its_guide_on_outdir` (P3.5c4): the
//!   actions and their run time checked on the live session.
//!
//! Nothing these tests write is committed.

use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api;
use game_logic::api::ai::{BaseOp, CallbackArg, Op, SetPersonalityParams};
use game_logic::content::Instance;
use game_logic::content::guides;
use game_logic::content::mission::action::{self, Action};
use game_logic::content::mission::dialog::{DialogLine, MissionDialog};
use game_logic::content::mission::execute;
use game_logic::content::mission::meta_behavior::{MbGuide, MetaBehavior};
use game_logic::content::mission::nags::NagKind;
use game_logic::content::mission::obj_list::{self, DirectiveKind, Goto, Idle, ObjListDirective};
use game_logic::content::mission::runtime;
use game_logic::content::mission::trace::{Trace, TraceRecord};
use game_logic::content::mission::trigger::Trigger;
use game_logic::content::mission::{MissionScript, engine, object_save, spawn};
use game_logic::content::npc_ships;
use game_logic::content::players::{PlayerShip, PlayerState};
use game_logic::content::ships::{self, ShipKey};
use game_logic::content::systems::PendingComm;
use game_logic::events::{CommResult, ControllerMessage, NpcShipKey};
use game_logic::server::{ClientSink, ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_logic::{selection, world};
use game_sim::ai::state_graph::GraphType;
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, ShipSpawn};
use game_sim::protocol::{CommonMessage, ServerMessage, ToClient};
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
    /// A `CombatState`'s `CSNType` and its two ids (reported, not
    /// compared: the ids are the bus's counter, which the oracle's
    /// harness controller shifts).
    combat: Option<(u32, u32, u32)>,
}

/// One object after a frame (`mission_objects.jsonl`'s `obj`).
#[derive(Clone, Debug, PartialEq)]
struct ObjRecord {
    tick: u32,
    kind: &'static str,
    name: String,
    /// The object's id (not compared; it names a `CombatState`'s ids).
    id: u32,
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
    let combat = match message {
        ControllerMessage::CombatState {
            state,
            param1,
            param2,
            ..
        } => Some((*state, *param1, *param2)),
        _ => None,
    };
    DELIVERIES.with(|d| {
        d.borrow_mut().push(Delivery {
            tick,
            depth,
            flid: id.0,
            msg,
            elapsed,
            combat,
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
                id: object,
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

/// A look at the server after a tick's frame (`probe(cx, mission, tick)`).
type Probe<'p> = &'p mut dyn FnMut(&mut Cx<'_>, ControllerId, u32);

/// The session's ticks after a frame: no look.
fn no_probe(_: &mut Cx<'_>, _: ControllerId, _: u32) {}

/// The session on a data root (`exe` holds `OutpostFrontier.ini`), with
/// `probe` after each tick's frame.
#[allow(clippy::too_many_lines)]
fn run_session(
    exe: &Path,
    data_dir: &Path,
    file: &SessionFile,
    probe: Probe<'_>,
) -> Option<Session> {
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
    // `comms off`: the GCS clamp, as the oracle's `OracleSimSetGcsClamp`
    // sets it before the system's load, so Content's comm system admits
    // nothing and draws no roll (P3.6b).
    if s.comms_off {
        cx.logic.content.state.clamps.set_gcs_clamp_enabled(1);
    }

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
    // The harness steps its scenario's system, whose player has no client
    // (`GOracleSimHooks.StepSystemId`, set after the mission's start).
    cx.logic.server_world.step_system = system;

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
        probe(&mut cx, id, tick);
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

/// The `CombatState`s the mission was handed (one per broadcast), as
/// `tick T csn C <owner> -> <target>`, each id named by the mission's
/// objects (`?<id>` for one that is not, `-` for 0). Reported, not
/// compared.
fn combat_lines(deliveries: &[Delivery], mission: u32, objects: &[ObjRecord]) -> Vec<String> {
    let names: BTreeMap<u32, &str> = objects.iter().map(|o| (o.id, o.name.as_str())).collect();
    let name = |id: u32| match id {
        0 => "-".to_owned(),
        _ => names
            .get(&id)
            .map_or_else(|| format!("?{id}"), |n| (*n).to_owned()),
    };
    deliveries
        .iter()
        .filter(|d| d.flid == mission)
        .filter_map(|d| {
            let (csn, owner, target) = d.combat?;
            Some(format!(
                "tick {} csn {csn} {} -> {}",
                d.tick,
                name(owner),
                name(target)
            ))
        })
        .collect()
}

/// Records shown on each side of a first difference.
const REPORT_WINDOW: usize = 4;

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
                combat: r
                    .get("csn")
                    .and_then(Json::as_u32)
                    .and_then(|csn| Some((csn, r.get("p1")?.as_u32()?, r.get("p2")?.as_u32()?))),
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
                id: r.get("id").and_then(Json::as_u32).unwrap_or(0),
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

/// The trace records of the judged window: up to and including the first
/// action that runs without an effect (all of them without one).
fn judged_records(ours: &Session) -> usize {
    ours.unported
        .first()
        .and_then(|&(tick, trigger, index, _)| {
            ours.trace.iter().position(|r| {
                matches!(*r, TraceRecord::Action { tick: t, trigger: g, index: i } | TraceRecord::Timer { tick: t, trigger: g, index: i } if t == tick && g == trigger && i == index)
            })
        })
        .map_or(usize::MAX, |at| at + 1)
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
    let Some(ours) = run_session(&root.join("EXE"), &root.join("DATA"), &file, &mut no_probe)
    else {
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
    let judged_records = judged_records(&ours);
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
    // The combat states (P3.5c4; reported): who reported which state.
    let ours_c = combat_lines(&ours.deliveries, ours.mission, &ours.objects);
    let their_c = combat_lines(&their_all, their_mission, &their_objects);
    let _ = writeln!(
        summary,
        "combat states: rust {} oracle {}",
        ours_c.len(),
        their_c.len()
    );
    for i in 0..ours_c.len().max(their_c.len()) {
        let r = ours_c.get(i).map_or("-", String::as_str);
        let o = their_c.get(i).map_or("-", String::as_str);
        let mark = if r == o { " " } else { ">" };
        let _ = writeln!(summary, "{mark} {i}: rust {r} | oracle {o}");
    }
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
    let Some(ours) = run_session(&root, &root.join("data"), &file, &mut no_probe) else {
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
    // P3.5c4's comms, dialogs and scene scripts have their effect, and no
    // action of P3.5c5 or P3.6d runs in the session's 900 ticks: the
    // judged window is the whole session.
    assert!(
        ours.unported.is_empty(),
        "actions without an effect: {}",
        unported_summary(&ours.unported)
    );
    // The opening's dialogs start on their timers and on the scripted
    // `CommComplete`s: the rendezvous (5 s after tick 2), Samura's
    // acknowledgement (the first line's end, tick 200) and the escorts'
    // launch (3 s after the second line's end, tick 260).
    for (trigger, tick) in [
        (&b"fp7_rendezvous_chatter_transport"[..], 152),
        (b"Samura_Ack_dlg", 200),
        (b"Base_sends_escorts", 350),
        (b"fp7_attack_dialog", 320),
    ] {
        assert_eq!(
            fired(id(trigger)).map(|f| f.0),
            Some(tick),
            "{}",
            String::from_utf8_lossy(trigger)
        );
    }
    println!(
        "{} trace records ({}); judged window: the whole session",
        ours.trace.len(),
        counts(&ours.trace),
    );
}

/// The NPC ship of the mission by its nickname.
fn npc_of(cx: &Cx<'_>, id: ControllerId, name: &[u8]) -> Option<NpcShipKey> {
    script_of(cx, id).and_then(|s| runtime::find_npc_by_name(cx, s, name))
}

/// The attitude of object `a`'s reputation towards object `b`'s.
fn attitude(cx: &mut Cx<'_>, a: u32, b: u32) -> Option<f32> {
    let ra = api::space_obj::rep(cx, a).ok()?;
    let rb = api::space_obj::rep(cx, b).ok()?;
    api::reputation::attitude(cx, ra, rb).ok()
}

/// The kind of the ship's active directive.
fn active_kind(cx: &Cx<'_>, ship: ShipKey) -> Option<DirectiveKind> {
    let q = npc_ships::queue(cx, ship)?;
    let d = q.active?;
    cx.logic
        .content
        .state
        .directives
        .get(d)
        .map(|d| d.kind.clone())
}

/// Runs `action` on the mission as its trigger's first action would.
fn run(cx: &mut Cx<'_>, id: ControllerId, mut action: Action) -> bool {
    execute::execute(cx, id, 0, 0, &mut action)
}

/// P3.5c3's actions and directives on `outdir/` (`MissionAct.cpp`,
/// `MissionObjList.cpp`): `Mission_01a`'s own vibes, object lists and
/// lives of tick 2, then, after tick 3's frame, each of the 13 actions run
/// on the live mission, checked against what the C++'s `Execute` does.
#[test]
#[allow(clippy::too_many_lines)]
fn the_ai_and_reputation_actions_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let session = file.mission.1;
    let checked = Cell::new(false);
    let mut probe = |cx: &mut Cx<'_>, id: ControllerId, tick: u32| {
        if tick != 3 {
            return;
        }
        checked.set(true);
        let player_ship = cx
            .logic
            .content
            .state
            .players
            .get(&session)
            .and_then(|p| p.ship.as_ref())
            .map_or(0, |s| s.ship);
        assert_ne!(player_ship, 0);
        let escort = |cx: &Cx<'_>, n: u8| {
            let name = format!("fp7base_escort{n}");
            npc_of(cx, id, name.as_bytes()).expect("the escort")
        };
        let (e1, e2, e3) = (escort(cx, 1), escort(cx, 2), escort(cx, 3));
        let flid = |cx: &Cx<'_>, k: NpcShipKey| npc_ships::space_obj(cx, k);

        // Tick 2's `Act_SetVibe = fp7base_escortN, Player, REP_FRIEND_MAXIMUM`
        // ran: the harness's ship has no vibe (-1) and its session no
        // player vibe, so the vibes towards `Player` set nothing, in the
        // C++'s `SetObjectAttitude` as in the port.
        assert_eq!(api::space_obj::rep(cx, player_ship).ok(), Some(-1));
        assert!(api::player::rep(cx, session).is_err());
        assert_eq!(attitude(cx, flid(cx, e1), player_ship), None);
        // `FP7_attackers_hold_position`: `Avoidance` submits its state and
        // advances, the `Delay` stays active.
        assert!(matches!(
            active_kind(cx, ShipKey::Npc(e1)),
            Some(DirectiveKind::Delay(d)) if d.delay_seconds == 600.0
        ));
        assert_eq!(
            npc_ships::queue(cx, ShipKey::Npc(e1)).map(|q| q.list.len()),
            Some(1)
        );
        // `transport_creeps`: `SetLifetime = 1000`, then the goto.
        let transport = npc_of(cx, id, b"fp7_transport1").expect("the transport");
        let t = npc_ships::get(cx, transport).expect("the transport");
        assert_eq!((t.life_time, t.life_left), (1000.0, 1000.0));
        assert!(matches!(
            active_kind(cx, ShipKey::Npc(transport)),
            Some(DirectiveKind::GotoVec(_))
        ));

        // `Act_SetRep`: the player's and an object's reputation towards a
        // group; an unknown label or group answers false.
        let group = api::reputation::reputation_group(cx, b"li_lsf_grp").expect("li_lsf_grp");
        let set_rep = |label: &[u8], group_id: u32, value: f32| {
            Action::SetRep(action::SetRep {
                label: label.to_vec(),
                faction: Vec::new(),
                group_id,
                value,
            })
        };
        // `Player` with a live player asks the session's vibe, which the
        // harness's has not: nothing is set, and the action answers true.
        assert!(run(cx, id, set_rep(b"Player", group, 0.25)));
        // An object's: `pub::Reputation::SetReputation(its vibe, group,
        // value)`, which the same call made again leaves as it is.
        let e1_rep = api::space_obj::rep(cx, flid(cx, e1)).expect("the escort's vibe");
        let before = api::reputation::reputation(cx, e1_rep, group).ok();
        assert!(run(cx, id, set_rep(b"fp7base_escort1", group, -0.5)));
        let after = api::reputation::reputation(cx, e1_rep, group).ok();
        assert_ne!(after, before);
        api::reputation::set_reputation(cx, e1_rep, group, -0.5).expect("the vibe");
        assert_eq!(api::reputation::reputation(cx, e1_rep, group).ok(), after);
        assert!(!run(cx, id, set_rep(b"nobody_here", group, 0.0)));
        assert!(!run(cx, id, set_rep(b"Player", 0, 0.0)));

        // `Act_RepChangeRequest`: the pop-up's state.
        assert!(run(
            cx,
            id,
            Action::RepChangeRequest(action::RepChangeRequest {
                shown: false,
                armed: true,
                system_id: create_id(b"li01"),
                groups: vec![group],
            })
        ));
        let s = script_of(cx, id).expect("the mission");
        assert!(s.rep_pop_up_armed && !s.rep_pop_up_shown);
        assert_eq!(s.rep_pop_up_system, create_id(b"li01"));
        assert_eq!(s.rep_pop_up_groups, vec![group]);

        // `Act_SetPriority`: every NPC ship under the label.
        assert!(run(
            cx,
            id,
            Action::SetPriority(action::SetPriority {
                formation_id: create_id(b"lbl_fp7_convoy"),
                priority: 1,
            })
        ));
        for k in [e1, e2, e3] {
            assert_eq!(npc_ships::priority(cx, ShipKey::Npc(k)), 1);
        }

        // `Act_SetFlee`: a position, an object (the label's), then clear.
        let set_flee = |mode: i32, target: &[u8], flag: bool| {
            Action::SetFlee(action::SetFlee {
                npc: b"fp7base_escort1".to_vec(),
                mode,
                position: Vector::new(1.0, 2.0, 3.0),
                target: target.to_vec(),
                distance: 500.0,
                flag,
            })
        };
        run(cx, id, set_flee(0, b"", false));
        let n = npc_ships::get(cx, e1).expect("the escort");
        assert_eq!(
            (n.flee_state, n.flee_position, n.flee_distance),
            (1, Vector::new(1.0, 2.0, 3.0), 500.0)
        );
        run(cx, id, set_flee(1, b"fp7base", true));
        let base = api::misc::nickname_id(b"fp7base");
        let n = npc_ships::get(cx, e1).expect("the escort");
        assert_eq!((n.flee_state, n.flee_object), (3, base));
        run(cx, id, set_flee(2, b"", false));
        assert_eq!(npc_ships::get(cx, e1).map(|n| n.flee_state), Some(0));

        // `Act_PilotParams`: `no_flee` clears the job's flee fields, then
        // `normal_flee` takes them from the archetype's pilot (the ship's
        // own pilot is empty, as in the C++).
        let pilot = |mode: i32| {
            Action::PilotParams(action::PilotParams {
                flee_mode: mode,
                label: b"fp7base_escort1".to_vec(),
            })
        };
        run(cx, id, pilot(0));
        let job = api::ai::personality(cx, flid(cx, e1))
            .expect("the personality")
            .job;
        assert_eq!(
            (job.flee_scene_threat_style, job.scene_toughness_threshold),
            (4, 4)
        );
        run(cx, id, pilot(1));
        let archetype = npc_ships::get(cx, e1).map_or(0, |n| n.base.archetype);
        let tables = cx.logic.content.tables.clone();
        let mut expected = game_sim::ai::personality::Personality::default();
        let loaded = tables
            .npc_ships
            .values()
            .find(|a| a.ship_archetype == archetype)
            .is_some_and(|a| tables.pilots.personality(a.pilot, &mut expected));
        let job = api::ai::personality(cx, flid(cx, e1))
            .expect("the personality")
            .job;
        if loaded {
            assert_eq!(
                job.flee_scene_threat_style,
                expected.job.flee_scene_threat_style
            );
            assert_eq!(
                job.scene_toughness_threshold,
                expected.job.scene_toughness_threshold
            );
            assert_eq!(job.loot_preference, expected.job.loot_preference);
        }

        // `Act_SetVibeOfferBaseHack`: the base's solar reputation towards
        // the player's; without the session's vibe nothing is set.
        let solar_rep = api::space_obj::solar_rep(cx, base);
        let before = solar_rep.map(|r| api::reputation::attitude(cx, r, -1));
        assert!(run(
            cx,
            id,
            Action::SetVibeOfferBaseHack(action::SetVibeOfferBaseHack { base_id: base })
        ));
        assert_eq!(
            solar_rep.map(|r| api::reputation::attitude(cx, r, -1)),
            before
        );

        // `Act_SetVibe` with a range draws one `rand()` between its ends.
        assert!(run(
            cx,
            id,
            Action::SetVibe(action::SetVibe {
                label_a: b"fp7base_escort2".to_vec(),
                label_b: b"fp7_transport1".to_vec(),
                lo: -0.5,
                hi: 0.5,
            })
        ));
        let a = attitude(cx, flid(cx, e2), flid(cx, transport)).expect("an attitude");
        assert!((-0.5..=0.5).contains(&a), "{a}");

        // `Act_SetVibeShipToLbl`: the ship towards every member under the
        // label.
        assert!(run(
            cx,
            id,
            Action::SetVibeShipToLbl(action::SetVibeShipToLbl {
                ship: b"fp7_transport1".to_vec(),
                formation_id: create_id(b"lbl_fp7_convoy"),
                lo: -0.75,
                hi: -0.75,
            })
        ));
        let t = flid(cx, transport);
        for k in [e1, e2, e3] {
            assert_eq!(attitude(cx, t, flid(cx, k)), Some(-0.75));
        }

        // `Act_PlayerForm`: the player's ship and the NPC ship form the
        // formation (`convoy1` has a player slot).
        let formation = create_id(b"convoy1");
        assert!(cx.logic.content.tables.formation(formation).is_some());
        run(
            cx,
            id,
            Action::PlayerForm(action::PlayerForm {
                npc: b"fp7base_escort2".to_vec(),
                formation: Some(formation),
            }),
        );
        let state = &cx.logic.content.state;
        let formed = ships::formation_id(state, ShipKey::Player(session));
        assert_ne!(formed, u32::MAX);
        assert_eq!(ships::formation_id(state, ShipKey::Npc(e2)), formed);

        // `Act_GiveMB`: the NPC ship cancels its directives and takes the
        // goto guide of the `[MetaBehavior]`'s first object.
        let mb = create_id(b"mb_p3_5c3_test");
        if let Some(s) = runtime::script_mut(cx, id) {
            s.object.meta_behaviors.insert(
                mb,
                vec![MetaBehavior::GotoGuide(MbGuide {
                    target_name: b"Player".to_vec(),
                    destination: Vector::new(3600.0, 1000.0, -21000.0),
                    range: 500.0,
                    ..MbGuide::default()
                })],
            );
        }
        run(
            cx,
            id,
            Action::GiveMb(action::GiveMb {
                npc: b"fp7base_escort3".to_vec(),
                list_id: mb,
            }),
        );
        let guide = npc_ships::get(cx, e3)
            .and_then(|n| n.guide)
            .expect("a guide");
        assert_eq!(guides::guide_type(cx, guide), 5);

        // `Act_GiveObjList` with named targets: a name that resolves is
        // written into the shared op; one that finds nothing finishes its
        // directive, and the ship goes on to the next.
        let named = create_id(b"ol_p3_5c3_named");
        let missing = create_id(b"ol_p3_5c3_missing");
        // `GotoShip = goto, <target>, 1000, true`, as `FObjListGotoShip::Parse`
        // makes it.
        let goto_ship = |target: &[u8]| {
            let mut goto = Goto {
                field_0x17c: 1,
                target_name: target.to_vec(),
                ..Goto::default()
            };
            goto.op.kind = 0;
            goto.op.range = 1000.0;
            goto.op.field_58 = 1;
            ObjListDirective {
                name: b"GotoShip".to_vec(),
                kind: DirectiveKind::GotoShip(goto),
            }
        };
        let idle = ObjListDirective {
            name: b"Idle".to_vec(),
            kind: DirectiveKind::Idle(Idle::default()),
        };
        if let Some(s) = runtime::script_mut(cx, id) {
            s.object
                .obj_lists
                .insert(named, vec![goto_ship(b"fp7base"), idle.clone()]);
            s.object
                .obj_lists
                .insert(missing, vec![goto_ship(b"no_such_object"), idle]);
        }
        let give = |npc: &[u8], list_id: u32| {
            Action::GiveObjList(action::GiveObjList {
                npc: npc.to_vec(),
                list_id,
            })
        };
        run(cx, id, give(b"fp7base_escort3", named));
        assert!(npc_ships::get(cx, e3).is_some_and(|n| n.guide.is_none()));
        assert!(matches!(
            active_kind(cx, ShipKey::Npc(e3)),
            Some(DirectiveKind::GotoShip(g)) if g.op.field_1c.cast_unsigned() == base
        ));
        let shared = script_of(cx, id)
            .and_then(|s| s.object.obj_list_refs.get(&named))
            .and_then(|l| l.first().copied())
            .expect("the shared list");
        assert!(matches!(
            cx.logic.content.state.directives.get(shared).map(|d| &d.kind),
            Some(DirectiveKind::GotoShip(g)) if g.op.field_1c.cast_unsigned() == base
        ));
        run(cx, id, give(b"fp7base_escort1", missing));
        assert!(matches!(
            active_kind(cx, ShipKey::Npc(e1)),
            Some(DirectiveKind::Idle(_))
        ));
        assert_eq!(
            npc_ships::queue(cx, ShipKey::Npc(e1)).map(|q| q.list.len()),
            Some(1)
        );

        // The mission kinds of the shipped lists: `SetPriority`,
        // `MakeNewFormation` and `StayInRange` run and advance; `Follow`
        // takes its named target and stays.
        let kinds = create_id(b"ol_p3_5c3_kinds");
        let mut zone = obj_list::Zone {
            object_name: b"fp7base".to_vec(),
            ..obj_list::Zone::default()
        };
        zone.op.mode = 2;
        zone.op.invert_mode = 0;
        zone.op.radius = 1000.0;
        let list = vec![
            ObjListDirective {
                name: b"SetPriority".to_vec(),
                kind: DirectiveKind::SetPriority(obj_list::SetPriority { priority: 0 }),
            },
            ObjListDirective {
                name: b"MakeNewFormation".to_vec(),
                kind: DirectiveKind::MakeNewFormation(obj_list::Formation {
                    formation: Some(create_id(b"convoy1")),
                    member_names: vec![b"fp7base_escort3".to_vec()],
                    add_followers: false,
                }),
            },
            ObjListDirective {
                name: b"StayInRange".to_vec(),
                kind: DirectiveKind::StayInRange(zone),
            },
            ObjListDirective {
                name: b"Follow".to_vec(),
                kind: DirectiveKind::Follow(obj_list::Follow {
                    field_0x10c: 1,
                    target_name: b"fp7_transport1".to_vec(),
                    ..obj_list::Follow::default()
                }),
            },
        ];
        if let Some(s) = runtime::script_mut(cx, id) {
            s.object.obj_lists.insert(kinds, list);
        }
        run(cx, id, give(b"fp7base_escort1", kinds));
        assert_eq!(npc_ships::priority(cx, ShipKey::Npc(e1)), 0);
        let state = &cx.logic.content.state;
        let formed = ships::formation_id(state, ShipKey::Npc(e1));
        assert_ne!(formed, u32::MAX);
        assert_eq!(ships::formation_id(state, ShipKey::Npc(e3)), formed);
        assert!(npc_ships::has_no_directives(cx, ShipKey::Npc(e3)));
        let refs = script_of(cx, id)
            .and_then(|s| s.object.obj_list_refs.get(&kinds).cloned())
            .expect("the shared list");
        assert!(matches!(
            cx.logic.content.state.directives.get(refs[2]).map(|d| &d.kind),
            Some(DirectiveKind::StayInRange(z)) if z.op.target_id == base
        ));
        assert!(matches!(
            active_kind(cx, ShipKey::Npc(e1)),
            Some(DirectiveKind::Follow(f)) if f.op.target == flid(cx, transport)
        ));
        assert_eq!(
            npc_ships::queue(cx, ShipKey::Npc(e1)).map(|q| q.list.len()),
            Some(1)
        );

        // `Act_GiveObjList` for `Player`: the player's ship object's queue
        // (no directive callback); `Avoidance` advances, the `Delay` stays.
        run(
            cx,
            id,
            give(b"Player", create_id(b"FP7_attackers_hold_position")),
        );
        assert!(matches!(
            active_kind(cx, ShipKey::Player(session)),
            Some(DirectiveKind::Delay(_))
        ));
        // A label that is no NPC and not `Player` gives nothing.
        run(cx, id, give(b"nobody_here", named));
    };
    if run_session(&root, &root.join("data"), &file, &mut probe).is_none() {
        println!("notice: outdir/ is missing or the session could not start; it was not run");
        return;
    }
    assert!(checked.get(), "the probe ran at tick 3");
}

// ---- P3.5c4: the dialogs, the cut scenes and the guides' hand-over ------------------------------

/// A client sink the probes read (the session's own is boxed away).
#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl Shared {
    fn take(&self) -> Vec<(u32, ToClient)> {
        std::mem::take(&mut *self.0.borrow_mut())
    }
}

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

/// One comm among the sent messages: client, speaker, addressee, texts.
type SentComm = (u32, u32, u32, Vec<u32>);

/// The comms among the sent messages.
fn comms(sent: &[(u32, ToClient)]) -> Vec<SentComm> {
    sent.iter()
        .filter_map(|(client, m)| match m {
            ToClient::Server(ServerMessage::SendComm(c)) => {
                Some((*client, c.field_20, c.field_24, c.text_ids.clone()))
            }
            _ => None,
        })
        .collect()
}

/// The firings of a trigger in the mission's trace: tick and event.
fn firings(cx: &Cx<'_>, id: ControllerId, trigger: u32) -> Vec<(u32, u32)> {
    script_of(cx, id)
        .and_then(|s| s.trace.as_ref())
        .map(|t| {
            t.records
                .iter()
                .filter_map(|r| match *r {
                    TraceRecord::Fire {
                        tick,
                        trigger: g,
                        event,
                    } if g == trigger => Some((tick, event)),
                    _ => None,
                })
                .collect()
        })
        .unwrap_or_default()
}

/// The space object of the session's ship.
fn session_ship(cx: &Cx<'_>, session: u32) -> u32 {
    ships::id(&cx.logic.content.state, ShipKey::Player(session))
}

/// The pending comms of a ship.
fn pending(cx: &Cx<'_>, ship: ShipKey) -> Option<Vec<PendingComm>> {
    ships::base(&cx.logic.content.state, ship).map(|s| s.pending_comms.clone())
}

/// A `CommComplete` of `line` through the bus, as the client reports a
/// comm's end (`IServerImpl::CommComplete`).
fn comm_done(cx: &mut Cx<'_>, listener: u32, system: u32, speaker: u32, line: u32) {
    let mut message = ControllerMessage::CommComplete {
        listener,
        system,
        speaker,
        line,
        result: CommResult::Success,
    };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
}

/// A dialog of two speakers in `system`: the transport's line `l1` to the
/// player, then the player's answer `l2`.
fn two_line_dialog(id: ControllerId, system: u32, l1: u32, l2: u32) -> MissionDialog {
    let mut dialog = MissionDialog::new(id);
    dialog.system = Some(system);
    dialog.speakers.insert(
        b"fp7_transport1".to_vec(),
        vec![DialogLine {
            speaker: b"fp7_transport1".to_vec(),
            target: b"Player".to_vec(),
            first: true,
            key: 0,
            text_id: l1,
            ..DialogLine::default()
        }],
    );
    dialog.speakers.insert(
        b"Player".to_vec(),
        vec![DialogLine {
            speaker: b"Player".to_vec(),
            target: b"fp7_transport1".to_vec(),
            first: false,
            key: l1,
            text_id: l2,
            ..DialogLine::default()
        }],
    );
    dialog
}

/// The mission dialogs on `outdir/` (`MissionDialogs.cpp`, `NpcBase.cpp`,
/// `SystemInfo.cpp`): `Mission_01a`'s rendezvous dialog of tick 152 said
/// its line from the transport to the player's ship and left it as the
/// transport's pending comm; a two-line dialog says its first line at once
/// and its second when the client reports the first one's end
/// (SpaceSupport's `SendPendingComms`); a dialog line's end reaches the
/// mission's `Cnd_CommComplete`, whose trigger starts the next dialog.
#[test]
fn the_dialogs_speak_and_complete_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let session = file.mission.1;
    let system = create_id(file.scenario.system.as_bytes());
    let sink = Shared::default();
    let checked = Cell::new(false);
    let mut probe = |cx: &mut Cx<'_>, id: ControllerId, tick: u32| {
        if tick == 1 {
            cx.logic.client = Box::new(sink.clone());
        }
        if tick == 151 {
            // The session has no client: the player gets one, and its view
            // takes every object it tracks as created (no client
            // acknowledges them), so the comms reach the client.
            if let Some(slot) = cx.logic.players.get_mut(session) {
                slot.client_id = session;
            }
            if let Some(r) = cx.logic.server_world.systems.get_mut(&system) {
                for view in &mut r.observers {
                    for t in &mut view.tracked {
                        t.created_on_client = true;
                    }
                }
            }
        }
        if tick != 160 {
            return;
        }
        checked.set(true);
        let ship = session_ship(cx, session);
        let key = npc_of(cx, id, b"fp7_transport1").expect("the transport");
        let transport = npc_ships::space_obj(cx, key);
        let line0 = create_id(b"DX_M01_0000_FREEPORT_7_BRIDGE");
        // `FP7_Rendezvous_transport` (tick 152): one line, said at once
        // and kept as the speaker's pending comm.
        let sent = comms(&sink.take());
        assert!(
            sent.contains(&(session, transport, ship, vec![line0])),
            "{sent:?}"
        );
        assert_eq!(
            pending(cx, ShipKey::Npc(key)),
            Some(vec![PendingComm {
                key: 0,
                text_id: line0,
                target_id: ship,
                field_0x0c: 0,
            }])
        );

        let (l1, l2) = (create_id(b"p3_5c4_line_one"), create_id(b"p3_5c4_line_two"));
        let dialog_id = create_id(b"p3_5c4_dialog");
        let dialog = two_line_dialog(id, system, l1, l2);
        cx.logic
            .content
            .state
            .mission_dialogs
            .insert(dialog_id, dialog);
        // The player in the record's players, as GameSupport's
        // `SpaceEnter` puts him.
        if let Some(r) = cx.logic.content.state.systems.get_mut(&system)
            && !r.players.contains(&session)
        {
            r.players.push(session);
        }
        assert!(run(
            cx,
            id,
            Action::StartDialog(action::StartDialog {
                dialog_id,
                ..action::StartDialog::default()
            })
        ));
        assert_eq!(
            comms(&sink.take()),
            vec![(session, transport, ship, vec![l1])]
        );
        assert_eq!(
            pending(cx, ShipKey::Player(session)),
            Some(vec![PendingComm {
                key: l1,
                text_id: l2,
                target_id: transport,
                field_0x0c: 0,
            }])
        );
        // The targets stay written into the dialog map.
        let written = cx.logic.content.state.mission_dialogs[&dialog_id].speakers[&b"Player"[..]]
            [0]
        .target_id;
        assert_eq!(written, transport);

        // The client reports the first line's end: SpaceSupport has the
        // player's ship say the second, which leaves its pending comms.
        comm_done(cx, ship, system, transport, l1);
        assert_eq!(
            comms(&sink.take()),
            vec![(session, ship, transport, vec![l2])]
        );
        assert_eq!(pending(cx, ShipKey::Player(session)), Some(Vec::new()));

        // The rendezvous line's end reaches the mission: `Samura_Ack_dlg`
        // (`Cnd_CommComplete = DX_M01_0000_FREEPORT_7_BRIDGE`) fires and
        // starts `Samura_Ack`, the transport's next line.
        comm_done(cx, ship, system, transport, line0);
        assert_eq!(
            firings(cx, id, create_id(b"Samura_Ack_dlg")),
            vec![(160, 0x0b)]
        );
        let line1 = create_id(b"DX_M01_0005_Samura_SA_14");
        assert!(comms(&sink.take()).contains(&(session, transport, ship, vec![line1])));
    };
    if run_session(&root, &root.join("data"), &file, &mut probe).is_none() {
        println!("notice: outdir/ is missing or the session could not start; it was not run");
        return;
    }
    assert!(checked.get(), "the probe ran at tick 160");
}

/// A cut scene on `outdir/` (`MissionScript.cpp`, `RtcScripts.cpp`,
/// `MissionCnd.cpp:2319-2336`): with the player on he01_01_Base,
/// `Act_AddRTC` of `Mission_01a`'s first cut scene loads its RTC script
/// and makes its bar scene with a new sequence id; `Cnd_RTCDone` matches
/// the `RtcDone` of that sequence id only, and its trigger's
/// `Act_PopUpDialog` reaches the client; `Act_RemoveRTC` finishes the
/// scene, and the base's exit releases it.
#[test]
#[allow(clippy::too_many_lines)]
fn a_cut_scene_starts_and_ends_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let session = file.mission.1;
    let sink = Shared::default();
    let checked = Cell::new(false);
    let mut probe = |cx: &mut Cx<'_>, id: ControllerId, tick: u32| {
        if tick != 3 {
            return;
        }
        checked.set(true);
        cx.logic.client = Box::new(sink.clone());
        let base = api::misc::base_id(cx, b"he01_01_Base").expect("the base");
        assert!(cx.logic.content.state.bases.contains_key(&base));
        if let Some(p) = cx.logic.content.state.players.get_mut(&session) {
            p.base = Some(base);
        }
        // As `Act_AddRTC = missions\\m01a\\M000_s002xe_Li01_01_nrml.ini` reads it.
        let path = b"missions\\\\m01a\\\\M000_s002xe_Li01_01_nrml.ini".to_vec();
        assert!(run(
            cx,
            id,
            Action::AddRtc(action::AddRtc {
                path: path.clone(),
                repeatable: false,
            })
        ));
        let rtc = script_of(cx, id)
            .and_then(|s| s.rtc_scripts.last().cloned())
            .expect("the cut scene");
        let loaded = rtc.script.as_deref().expect("started");
        let location = api::gf::find_location(base, b"Cityscape");
        assert_eq!((loaded.location, loaded.base_id), (location, base));
        assert_eq!(loaded.roles.len(), 1);
        assert_eq!(loaded.roles[0].role, b"Lonnigan");
        let sequence = loaded.sequence_id;
        assert_ne!(sequence, 0);
        assert_ne!(rtc.handle, -1);
        let scene = cx.logic.content.state.bases[&base]
            .bar_scenes
            .iter()
            .find(|b| b.scene_id == rtc.handle)
            .cloned()
            .expect("the bar scene");
        assert_eq!((scene.player, scene.room), (session, location));
        assert_eq!(scene.characters.len(), 1);

        // `rtcd_cityscape` (`Cnd_RTCDone` of the file): another sequence
        // id does not match, the scene's does.
        let trigger = create_id(b"rtcd_cityscape");
        engine::activate_trigger(cx, id, trigger);
        let _ = sink.take();
        let done = |cx: &mut Cx<'_>, rtc: u32| {
            let mut message = ControllerMessage::RtcDone {
                player: session,
                rtc,
            };
            let _ = cx.dispatch(id, &mut message);
        };
        done(cx, sequence.wrapping_add(1));
        assert!(firings(cx, id, trigger).is_empty());
        done(cx, sequence);
        assert_eq!(firings(cx, id, trigger), vec![(3, 0x22)]);
        // Its `Act_Popupdialog = 21891, 21925, CLOSE`.
        assert!(sink.take().iter().any(|(client, m)| *client == session
            && matches!(m, ToClient::Common(CommonMessage::PopUpDialog(p)) if p.field_10 == 8)));

        // `Act_RemoveRTC` in the scene's room: the scene's character loses
        // its behaviour and the scene is finished.
        if let Some(s) = runtime::script_mut(cx, id) {
            s.current_location = location;
        }
        let character = scene.characters[0];
        assert!(run(
            cx,
            id,
            Action::RemoveRtc(action::RemoveRtc { rtc_file: path })
        ));
        assert!(
            script_of(cx, id)
                .and_then(|s| s.rtc_scripts.last())
                .is_some_and(|r| r.finished)
        );
        assert_eq!(
            game_logic::content::bar::character(cx, base, character).map(|c| c.behavior),
            Some(u32::MAX)
        );
        // The base's exit releases the bar scene and unlinks the finished
        // record.
        let mut exit = ControllerMessage::BaseExit {
            base,
            player: session,
        };
        let _ = cx.dispatch(id, &mut exit);
        assert!(script_of(cx, id).is_some_and(|s| s.rtc_scripts.is_empty()));
        assert!(
            !cx.logic.content.state.bases[&base]
                .bar_scenes
                .iter()
                .any(|b| b.scene_id == rtc.handle)
        );
    };
    if run_session(&root, &root.join("data"), &file, &mut probe).is_none() {
        println!("notice: outdir/ is missing or the session could not start; it was not run");
        return;
    }
    assert!(checked.get(), "the probe ran at tick 3");
}

/// The guide's hand-over on `outdir/` (`MissionSave.cpp:77-95`,
/// `MissionObject.cpp:2540-2556`): the snapshot of a guided mission ship
/// gives its respawn record the ship's guide (one more reference, the
/// guide's ship taken away), the save writes the guide's line, and the
/// respawn gives the new ship the same guide, the record's reference
/// released.
#[test]
#[allow(clippy::too_many_lines)]
fn a_guided_ship_respawns_with_its_guide_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let held: Cell<Option<guides::GuideRef>> = Cell::new(None);
    let checked = Cell::new(false);
    let line = b"MB_GOTOGUIDE = p3_5c4_goto".to_vec();
    let mut probe = |cx: &mut Cx<'_>, id: ControllerId, tick: u32| match tick {
        3 => {
            let mb = create_id(b"mb_p3_5c4_test");
            if let Some(s) = runtime::script_mut(cx, id) {
                s.object.meta_behaviors.insert(
                    mb,
                    vec![MetaBehavior::GotoGuide(MbGuide {
                        line: line.clone(),
                        target_name: b"Player".to_vec(),
                        destination: Vector::new(3600.0, 1000.0, -21000.0),
                        range: 500.0,
                        ..MbGuide::default()
                    })],
                );
            }
            run(
                cx,
                id,
                Action::GiveMb(action::GiveMb {
                    npc: b"fp7base_escort3".to_vec(),
                    list_id: mb,
                }),
            );
            let e3 = npc_of(cx, id, b"fp7base_escort3").expect("the escort");
            let guide = npc_ships::get(cx, e3)
                .and_then(|n| n.guide)
                .expect("a guide");
            spawn::snapshot_mission_objects(cx, id);
            let slot = script_of(cx, id)
                .and_then(|s| {
                    s.object
                        .respawn_ships
                        .iter()
                        .flat_map(|g| g.ships.iter())
                        .find(|s| s.nickname == b"fp7base_escort3")
                        .cloned()
                })
                .expect("the escort's record");
            assert_eq!(slot.guide_ref, Some(guide));
            assert!(slot.directives.is_empty());
            assert_eq!(cx.logic.content.state.guides.refs(guide), 2);
            assert_eq!(
                cx.logic.content.state.guides.get(guide).map(|g| g.ship),
                Some(None)
            );
            // The save writes the guide's line.
            let mut object = script_of(cx, id)
                .map(|s| s.object.clone())
                .expect("the mission");
            let mut out = Vec::new();
            object_save::write(cx, &mut out, &mut object).expect("in memory");
            let mut wanted = b"\n[MetaBehavior]\n".to_vec();
            wanted.extend_from_slice(&line);
            wanted.extend_from_slice(b"\n[MsnShipSave]\nnickname = fp7base_escort3\n");
            assert!(
                out.windows(wanted.len()).any(|w| w == wanted.as_slice()),
                "{}",
                String::from_utf8_lossy(&out)
            );
            // Only the escort's record respawns; the escort goes.
            if let Some(s) = runtime::script_mut(cx, id) {
                s.object
                    .respawn_ships
                    .retain(|g| g.ships.iter().any(|s| s.guide_ref.is_some()));
            }
            let ship = npc_ships::space_obj(cx, e3);
            assert!(api::space_obj::destroy(cx, ship, 0).is_ok());
            held.set(Some(guide));
        }
        4 => {
            let Some(guide) = held.get() else { return };
            checked.set(true);
            assert!(npc_of(cx, id, b"fp7base_escort3").is_none());
            assert_eq!(cx.logic.content.state.guides.refs(guide), 1);
            spawn::respawn_mission_objects(cx, id, true);
            let e3 = npc_of(cx, id, b"fp7base_escort3").expect("the escort again");
            assert_eq!(npc_ships::get(cx, e3).and_then(|n| n.guide), Some(guide));
            assert_eq!(
                cx.logic.content.state.guides.get(guide).map(|g| g.ship),
                Some(Some(e3))
            );
            assert_eq!(cx.logic.content.state.guides.refs(guide), 1);
            assert!(script_of(cx, id).is_some_and(|s| s.object.respawn_ships.is_empty()));
        }
        _ => {}
    };
    if run_session(&root, &root.join("data"), &file, &mut probe).is_none() {
        println!("notice: outdir/ is missing or the session could not start; it was not run");
        return;
    }
    assert!(checked.get(), "the probe ran at tick 4");
}

// ---- P3.5c5: the objectives, the NN path, the nags, the group and the end ----------------------

/// The `SETMISSIONOBJECTIVES` among the sent messages: client and the
/// objectives' state bytes.
fn objective_sends(sent: &[(u32, ToClient)]) -> Vec<(u32, Vec<u8>)> {
    sent.iter()
        .filter_map(|(client, m)| match m {
            ToClient::Server(ServerMessage::SetMissionObjectives(o)) => Some((
                *client,
                o.objectives.iter().map(|o| o.state).collect::<Vec<u8>>(),
            )),
            _ => None,
        })
        .collect()
}

/// The mission record's objective of `nickname`.
fn objective_of(
    cx: &Cx<'_>,
    id: ControllerId,
    player: u32,
    nickname: &[u8],
) -> Option<game_logic::content::mission::objective::Objective> {
    let nickname = create_id(nickname);
    script_of(cx, id)?
        .object
        .players
        .get(&player)?
        .objectives
        .iter()
        .find(|o| o.nickname == nickname)
        .cloned()
}

/// P3.5c5's actions and run time on `outdir/` (`MissionAct.cpp`,
/// `MissionObject.cpp`, `MissionScript.cpp`): `Mission_01a`'s
/// `[NNObjective]` records renumbered and sent hidden at `SpaceEnter`; then,
/// on the live mission, an objective given, advanced and hidden
/// (`Act_SetNNObj`, `Act_SetNNState`, `Act_SetNNHidden`,
/// `Act_GiveNNObjs`); an NN path started (`Act_NNPath`), laid out and sent
/// on its timer; a distance nag (`Act_NagDistTowards`) armed and saying its
/// line after 20 s; the player joining as a group member
/// (`GroupMemberJoined`); and the mission's success paying the
/// `[Mission]` reward, counting the completion and ending.
#[test]
#[allow(clippy::too_many_lines)]
fn the_objectives_path_nags_and_end_on_outdir() {
    let root = outdir();
    let Some(file) = read_session() else {
        println!("notice: CppSource/Oracle/mission_session.txt does not parse");
        return;
    };
    let session = file.mission.1;
    let sink = Shared::default();
    let nag_id = create_id(b"p3_5c5_nag");
    let checks = Cell::new(0u32);
    let mut probe = |cx: &mut Cx<'_>, id: ControllerId, tick: u32| {
        match tick {
            1 => {
                cx.logic.client = Box::new(sink.clone());
                if let Some(slot) = cx.logic.players.get_mut(session) {
                    slot.client_id = session;
                }
            }
            4 => {
                // `SpaceEnter` (tick 2): no NN path, so every record was
                // renumbered; every objective is hidden, so the record sent
                // an empty list.
                let s = script_of(cx, id).expect("the mission");
                let record = s.object.players.get(&session).expect("the record");
                assert_eq!(record.objectives.len(), 17, "{:?}", record.objectives);
                assert!(record.objectives.iter().all(|o| o.hidden && o.index == -1));
                let sent = objective_sends(&sink.take());
                assert!(sent.contains(&(session, Vec::new())), "{sent:?}");

                // `Act_SetNNObj = mlog_job`: the path reset, every objective
                // hidden, the objective's state 0x3a, shown, renumbered and
                // sent alone (new with 0x10: a clone kept).
                assert!(run(
                    cx,
                    id,
                    Action::SetNnObj(action::SetNnObj {
                        objective_id: create_id(b"mlog_job"),
                        state: 0x3a,
                    })
                ));
                let job = objective_of(cx, id, session, b"mlog_job").unwrap();
                assert!(!job.hidden);
                assert_eq!((job.index, job.state), (0, 0x3a));
                assert_eq!(job.description.mad_lib_id, 21810);
                assert_eq!(objective_sends(&sink.take()), [(session, vec![0x3a])]);
                let record = &script_of(cx, id).unwrap().object.players[&session];
                assert_eq!(record.sent.len(), 1);

                // `Act_SetNNState = mlog_job, COMPLETE`: its place's state
                // sent.
                assert!(run(
                    cx,
                    id,
                    Action::SetNnState(action::SetNnState {
                        objective_id: create_id(b"mlog_job"),
                        state: 3,
                    })
                ));
                let states: Vec<_> = sink
                    .take()
                    .into_iter()
                    .filter_map(|(c, m)| match m {
                        ToClient::Server(ServerMessage::SetMissionObjectiveState(s)) => {
                            Some((c, s.field_04, s.field_08))
                        }
                        _ => None,
                    })
                    .collect();
                assert_eq!(states, [(session, 0, 3)]);

                // `Act_SetNNHidden = mlog_job, true`, then `Act_GiveNNObjs`:
                // hidden, and the renumber sends an empty list.
                assert!(run(
                    cx,
                    id,
                    Action::SetNnHidden(action::SetNnHidden {
                        visible: false,
                        objective_id: create_id(b"mlog_job"),
                    })
                ));
                assert!(objective_of(cx, id, session, b"mlog_job").unwrap().hidden);
                assert!(run(cx, id, Action::GiveNnObjs(action::GiveNnObjs {})));
                assert_eq!(objective_sends(&sink.take()), [(session, Vec::new())]);
                assert_eq!(
                    objective_of(cx, id, session, b"mlog_job").unwrap().index,
                    -1
                );

                // `Act_NNPath` to a point 6 km from the player in its system.
                let ship = session_ship(cx, session);
                let (at, _) = api::space_obj::location(cx, ship).unwrap();
                let to = Vector::new(at.x + 6000.0, at.y, at.z);
                assert!(run(
                    cx,
                    id,
                    Action::NnPath(action::NnPath {
                        position: to,
                        label: Vec::new(),
                        system_id: 0,
                        travel_ids: 21900,
                        target_ids: 21901,
                    })
                ));
                let path = &script_of(cx, id).unwrap().nn_path;
                assert!(path.active);
                assert_eq!(path.state, 1);
                checks.set(checks.get() + 1);
            }
            50 => {
                // The first heartbeat started the path's 1 s timer (state
                // 2); its expiry rebuilt it: free flight to the point, one
                // type 5 objective of the target's ids (one leg in the
                // path's system: arrived, state 5), sent as 0x3a.
                let path = &script_of(cx, id).unwrap().nn_path;
                assert!(path.active);
                assert!(path.arrived);
                assert_eq!(path.state, 3);
                assert_eq!(path.objectives.len(), 1, "{:?}", path.objectives);
                let o = &path.objectives[0];
                assert_eq!((o.kind, o.ids), (5, 21901));
                let sent = objective_sends(&sink.take());
                assert_eq!(sent.first(), Some(&(session, vec![0x3a])), "{sent:?}");
                // The later sends (one a second) show it as 0xa.
                assert!(sent[1..].iter().all(|(_, s)| s == &[0xa]), "{sent:?}");

                // `Mission_01a` clamps its nags here: `Act_NagClamp = false`,
                // then `Act_NagDistTowards = POS, p3_5c5_nag, none, x, y, z`
                // with a distance of 10, the point 20 km off.
                assert!(run(
                    cx,
                    id,
                    Action::NagClamp(action::NagClamp {
                        clamp: false,
                        ..action::NagClamp::default()
                    })
                ));
                let ship = session_ship(cx, session);
                let (at, _) = api::space_obj::location(cx, ship).unwrap();
                assert!(run(
                    cx,
                    id,
                    Action::NagDistTowards(action::NagDistTowards {
                        nag_id,
                        speaker: b"none".to_vec(),
                        target: Vec::new(),
                        distance: 10.0,
                        fail_mad_lib_id: 1,
                        position: Vector::new(at.x, at.y, at.z + 20_000.0),
                        nag_in_combat: false,
                        nag_out_of_combat: true,
                    })
                ));
                assert!(script_of(cx, id).unwrap().nags.contains_key(&nag_id));
                checks.set(checks.get() + 1);
            }
            51 => {
                // The next heartbeat armed it: the player is farther than
                // 10.
                let nag = &script_of(cx, id).unwrap().nags[&nag_id];
                let NagKind::Dist(d) = &nag.kind else {
                    panic!("{nag:?}")
                };
                assert!(d.armed, "{nag:?}");
                assert_eq!((d.threshold, d.nags_left), (900.0, 4));
                checks.set(checks.get() + 1);
            }
            640 => {
                // The mission's own triggers clamped the nags again; the
                // timer comes at tick 651.
                assert!(run(
                    cx,
                    id,
                    Action::NagClamp(action::NagClamp {
                        clamp: false,
                        ..action::NagClamp::default()
                    })
                ));
            }
            700 => {
                // 20 s later the timer found the player no closer: the
                // position line (no speaker: the NN message `nag_pos_01-`)
                // and one nag spent, the timer set again.
                let nag = &script_of(cx, id).unwrap().nags[&nag_id];
                let NagKind::Dist(d) = &nag.kind else {
                    panic!("{nag:?}")
                };
                assert_eq!(d.nags_left, 3, "{nag:?}");
                assert!(d.armed);
                let line = create_id(b"nag_pos_01-");
                assert_eq!(nag.line, line);
                let nn: Vec<_> = sink
                    .take()
                    .into_iter()
                    .filter_map(|(c, m)| match m {
                        ToClient::Server(ServerMessage::SendNnMessage(m)) => Some((c, m.value)),
                        _ => None,
                    })
                    .collect();
                assert!(nn.contains(&(session, line)), "{nn:?}");

                // `GroupMemberJoined` of the player (`IsGroupMember` of the
                // player himself is true): `JoinMission`.
                let mut joined = ControllerMessage::GroupMemberJoined { player: session };
                let _ = api::controller::send_message(cx, id, &mut joined);
                assert_eq!(cx.logic.content.state.players[&session].live_mission, id.0);

                // The success: the reward paid to the player (no group; the
                // multiplayer server without a character counts it earned
                // and keeps the cash), the completion counted, the
                // objectives cleared and sent, the mission ended and left.
                let earned = |cx: &Cx<'_>| cx.logic.players.get(session).map(|p| p.total_earned);
                let before = earned(cx).unwrap();
                let reward = script_of(cx, id).unwrap().object.reward;
                assert_eq!(reward, 2000);
                if let Some(Instance::Mission(s)) = cx.logic.content.instances.get_mut(&id) {
                    engine::set_desired_state(s, engine::MISSION_STATE_SUCCEED, 1);
                }
                sink.take();
                engine::apply_desired_state(cx, id);
                assert_eq!(earned(cx), Some(before + 2000));
                let state = &cx.logic.content.state.players[&session];
                assert_eq!(state.stats.random_missions_completed.get(&7), Some(&1));
                assert_eq!(state.live_mission, 0);
                let s = script_of(cx, id).unwrap();
                assert!(s.object.players.is_empty());
                assert!(s.nags.is_empty());
                assert!(s.object.pending_destroy);
                let sent = objective_sends(&sink.take());
                assert!(sent.contains(&(session, Vec::new())), "{sent:?}");
                checks.set(checks.get() + 1);
            }
            _ => {}
        }
    };
    let ran = run_session(&root, &root.join("data"), &file, &mut probe);
    if ran.is_none() {
        println!("notice: outdir/OutpostFrontier.ini is missing; the session was not run");
        return;
    }
    assert_eq!(checks.get(), 4, "every probe ran");
}
