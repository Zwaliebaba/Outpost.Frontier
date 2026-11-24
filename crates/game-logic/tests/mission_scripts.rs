//! The mission scripts (work package P3.5a, decision record 0008 points 6
//! and 10, Gate G3): the fourteen story missions made as `Oracle.exe sim`
//! makes them for `CppSource/Oracle/mission_scripts.txt` and rendered as
//! `Content.dll`'s `OracleDumpMissionScripts` writes them
//! (`CppSource/Content/oracle_content_dump.cpp`), with P3.5a2's
//! `[ObjList]` and `[MetaBehavior]` tables.
//!
//! The port starts a multiplayer server as the oracle does (Content's
//! GameSupport, SpaceSupport and BaseSupport, then `Server_LoadWorldState`
//! over `InitialWorld.ini`), registers player 7 in he01, and for each
//! mission empties the mission dialogs, seeds the world's generator,
//! constructs the mission on a controller id no controller has (so its
//! heartbeats draw nothing, as in the oracle) and counts its draws.
//!
//! `data_dump_mission_scripts` reads `OUTPOST_DATA_DIR` (the dump root
//! with `EXE` and `DATA`) and `OUTPOST_ORACLE_DIR` (the oracle's output,
//! with the session's files in `missions/`); without the first it prints a
//! notice and passes, without the second it renders and compares nothing.
//! The other tests check the shipped data in `outdir/` and return early
//! when it is missing. Nothing these tests read or write is committed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::mission::action::Action;
use game_logic::content::mission::condition::Condition;
use game_logic::content::mission::field::{costume, hex, render_fields};
use game_logic::content::mission::objective::Objective;
use game_logic::content::mission::rand_enc::RandEncKind;
use game_logic::content::mission::{self, CHAPTERS, MissionScript};
use game_logic::content::players::PlayerState;
use game_logic::selection;
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::string_cache::StringCache;
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// The tables in the oracle's order, each `mission_<name>.txt`.
const TABLES: [&str; 18] = [
    "scripts",
    "triggers",
    "actions",
    "ship_groups",
    "ships",
    "solars",
    "loot",
    "rand_encs",
    "npcs",
    "npc_ship_archs",
    "objectives",
    "dialogs",
    "dialog_lines",
    "costumes",
    "obj_lists",
    "obj_list_directives",
    "meta_behaviors",
    "meta_behavior_objects",
];

/// `OracleMissionSeed`.
const SEED: u32 = 0x4f52_4143;
/// `OraclePlayerId`.
const PLAYER: u32 = 7;
/// `OracleControllerBase`.
const CONTROLLER_BASE: u32 = 0x00ff_0001;

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The kind of each of [`CHAPTERS`].
fn kind(index: usize) -> ControllerKind {
    match index {
        0 => ControllerKind::Mission01a,
        1 => ControllerKind::Mission01b,
        n => ControllerKind::Mission(u8::try_from(n).expect("a chapter index")),
    }
}

/// A multiplayer server over a data root, started as the oracle starts it:
/// Content's three controllers, then `Server_LoadWorldState`.
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

/// One mission made: the script, its draws and its rendered dialogs.
type Made = (MissionScript, i64, Vec<String>);

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

/// The fourteen missions as the oracle makes them, each with its draws.
fn make_missions(cx: &mut Cx<'_>) -> Vec<Made> {
    let he01 = api::misc::system_id(cx, b"he01").ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = he01.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
    let mut out = Vec::new();
    for (index, (name, _)) in CHAPTERS.iter().enumerate() {
        cx.logic.content.state.mission_dialogs.clear();
        cx.world.rng().seed(SEED);
        let before = cx.world.rng().clone();
        let script = mission::construct_script(
            cx,
            ControllerId(CONTROLLER_BASE + u32::try_from(index).unwrap()),
            kind(index),
            &CreateParams::StoryMission {
                player: PLAYER,
                story: ControllerId::BROADCAST,
            },
        );
        let after = cx.world.rng().clone();
        let dialogs = render_dialogs(name, cx);
        out.push((script, draws(&before, &after), dialogs));
    }
    cx.logic.content.state.mission_dialogs.clear();
    cx.logic.content.state.players.remove(&PLAYER);
    out
}

/// `mission_dialogs.txt` and `mission_dialog_lines.txt` of the dialogs
/// the mission left, in that order of the returned pair's halves: the
/// lines first, then the dialog headers after a separator entry.
fn render_dialogs(name: &str, cx: &Cx<'_>) -> Vec<String> {
    let strings = cx.world.strings();
    let mut lines = Vec::new();
    let mut headers = Vec::new();
    for (id, d) in &cx.logic.content.state.mission_dialogs {
        let mut count = 0;
        for (speaker, list) in &d.speakers {
            for (index, line) in list.iter().enumerate() {
                lines.push(format!(
                    "L mission={name} dialog={id} speaker_key={} index={index}{}",
                    hex(speaker),
                    render_fields(&line.fields(), strings)
                ));
                count += 1;
            }
        }
        headers.push(format!(
            "D mission={name} dialog={id} controller={} system={} speakers={} lines={count}",
            d.controller.0,
            d.system.unwrap_or(0),
            d.speakers.len()
        ));
    }
    lines.extend(headers);
    lines
}

fn render_condition(c: &Condition, strings: &StringCache) -> String {
    format!("{}{}", c.name(), render_fields(&c.fields(), strings))
}

fn render_action(a: &Action, strings: &StringCache) -> String {
    format!("{}{}", a.name(), render_fields(&a.fields(), strings))
}

fn render_objective(o: &Objective) -> String {
    let mut s = format!(
        " type={} hidden={} nickname={} ids={} index={} state={}",
        o.kind,
        u8::from(o.hidden),
        o.nickname,
        o.ids,
        o.index,
        o.state
    );
    let marker = |m: &[u32; 5]| {
        format!(
            " marker={}",
            m.iter().map(u32::to_string).collect::<Vec<_>>().join(",")
        )
    };
    match o.kind {
        1 => {
            let _ = write!(s, " base_id={}", o.base_id);
        }
        2 => s.push_str(&marker(&o.marker)),
        4 => {
            s.push_str(&marker(&o.marker));
            let _ = write!(s, " label={}", hex(&o.label));
        }
        5 | 6 => {
            let _ = write!(
                s,
                " rep_instance={} system_id={}{}",
                o.rep_instance,
                o.system_id,
                marker(&o.marker)
            );
        }
        _ => {}
    }
    s
}

/// The eighteen tables of one mission run.
fn render(missions: &[Made], strings: &StringCache) -> Vec<Vec<String>> {
    let mut out: Vec<Vec<String>> = vec![Vec::new(); TABLES.len()];
    for ((name, _), (s, draws, dialogs)) in CHAPTERS.iter().zip(missions) {
        let o = &s.object;
        let actions: usize = s.triggers.values().map(|t| t.actions.len()).sum();
        let objectives: usize = o.players.values().map(|p| p.objectives.len()).sum();
        let dialog_count = dialogs.iter().filter(|l| l.starts_with('D')).count();
        let initially_active = s
            .initially_active
            .iter()
            .map(|(k, v)| format!("{k}:{v}"))
            .collect::<Vec<_>>()
            .join(",");
        out[0].push(format!(
            "mission={name} draws={draws} controller={} player={} player_state={} players={} \
             heartbeat_interval={} reward={} title={} offer={} previous_state={} mission_state={} \
             field_0x2b0={} campaign_slot_current={} campaign_slot_succeed={} story_cue={} \
             field_0x2c0={} field_0x2c4={} field_0x2e8={} elapsed_time={} triggers={} actions={actions} \
             initially_active={initially_active} ship_groups={} solars={} loot={} rand_encs={} npcs={} \
             npc_ship_archs={} objectives={objectives} dialogs={dialog_count} dialog_costumes={}",
            o.controller.0,
            o.player,
            u8::from(o.player_state),
            o.players.len(),
            bits(o.heartbeat_interval),
            o.reward,
            o.title,
            o.offer,
            s.previous_state,
            s.mission_state,
            u8::from(s.field_0x2b0),
            s.campaign_slot_current,
            s.campaign_slot_succeed,
            s.story_cue,
            s.field_0x2c0,
            s.field_0x2c4,
            s.field_0x2e8,
            bits(s.elapsed_time),
            s.triggers.len(),
            o.ships.len(),
            o.solars.len(),
            o.loot.len(),
            o.rand_encs.len(),
            o.npcs.len(),
            o.npc_ship_archs.len(),
            s.dialog_costumes.len(),
        ));
        for (index, c) in s.dialog_costumes.iter().enumerate() {
            out[13].push(format!(
                "mission={name} index={index} costume={}",
                costume(c)
            ));
        }
        for (id, t) in &s.triggers {
            let events = t
                .events
                .iter()
                .map(u32::to_string)
                .collect::<Vec<_>>()
                .join(",");
            let condition = match &t.condition {
                Some(c) => format!(" cnd_trigger={id} cnd={}", render_condition(c, strings)),
                None => " cnd=none".to_owned(),
            };
            out[1].push(format!(
                "mission={name} trigger={id} state={} repeatable={} id={} events={events} actions={}{condition}",
                t.state,
                u8::from(t.repeatable),
                t.id,
                t.actions.len()
            ));
            for (index, a) in t.actions.iter().enumerate() {
                out[2].push(format!(
                    "mission={name} trigger={id} index={index} act_trigger={id} act={}",
                    render_action(a, strings)
                ));
            }
        }
        for (g, group) in o.ships.iter().enumerate() {
            out[3].push(format!(
                "mission={name} group={g}{} ships={}",
                render_fields(&group.fields(), strings),
                group.ships.len()
            ));
            for (index, ship) in group.ships.iter().enumerate() {
                out[4].push(format!(
                    "mission={name} group={g} index={index}{}",
                    render_fields(&ship.fields(), strings)
                ));
            }
        }
        for (index, solar) in o.solars.iter().enumerate() {
            out[5].push(format!(
                "mission={name} index={index}{}",
                render_fields(&solar.fields(), strings)
            ));
        }
        for (index, loot) in o.loot.iter().enumerate() {
            out[6].push(format!(
                "mission={name} index={index}{}",
                render_fields(&loot.fields(), strings)
            ));
        }
        for (index, r) in o.rand_encs.iter().enumerate() {
            let mut line = format!(
                "mission={name} index={index} nickname={} enabled={} nickname_id={} formation_index={} npc_count={}",
                hex(&r.nickname),
                u8::from(r.enabled),
                r.nickname_id,
                r.formation_index,
                r.npc_count
            );
            match &r.params {
                None => line.push_str(" params=0"),
                Some(p) => {
                    let positions = p
                        .positions
                        .iter()
                        .map(|(v, w)| {
                            format!("{},{},{},{}", bits(v.x), bits(v.y), bits(v.z), bits(*w))
                        })
                        .collect::<Vec<_>>()
                        .join(";");
                    let formations = p
                        .formations
                        .iter()
                        .map(|f| {
                            let choices = f
                                .ship_choices
                                .iter()
                                .map(|c| {
                                    format!(
                                        "{}/{}",
                                        c.ship_classes
                                            .iter()
                                            .map(u32::to_string)
                                            .collect::<Vec<_>>()
                                            .join(","),
                                        bits(c.weight)
                                    )
                                })
                                .collect::<Vec<_>>()
                                .join(";");
                            format!(
                                "{}:{}:{}:{}:{}:{}:{}:{choices}",
                                u8::from(f.formation.is_some()),
                                f.nickname,
                                bits(f.field_0x08),
                                f.min_count,
                                f.max_count,
                                f.count,
                                bits(f.weight)
                            )
                        })
                        .collect::<Vec<_>>()
                        .join("+");
                    let labels = p
                        .labels
                        .iter()
                        .map(u32::to_string)
                        .collect::<Vec<_>>()
                        .join(",");
                    let _ = write!(
                        line,
                        " params={} system={} controller={} player={} attacker_rep={} pilot_id={} \
                         labels={labels} positions={positions} ships_spawned={} formations_to_spawn={} \
                         num_forms_min={} num_forms_max={} formations={formations} target_ship_name={} \
                         goto_target_id={} target_object_id={} dock_target={} spawn_angle={} \
                         spawn_distance={} empty_pos_radius={} ship_name_counter={}",
                        p.kind.code(),
                        p.system.unwrap_or(0),
                        p.controller,
                        p.player.unwrap_or(0),
                        p.attacker_rep,
                        p.pilot_id,
                        p.ships_spawned,
                        p.formations_to_spawn,
                        p.num_forms_min,
                        p.num_forms_max,
                        hex(&p.target_ship_name),
                        p.goto_target_id,
                        p.target_object_id,
                        p.dock_target,
                        bits(p.spawn_angle),
                        bits(p.spawn_distance),
                        p.empty_pos_radius,
                        p.ship_name_counter
                    );
                    match p.kind {
                        RandEncKind::Space => {
                            let v = p.activation_location;
                            let _ = write!(
                                line,
                                " activation_type={} activation_location={},{},{} activation_range={} \
                                 activation_time_0={} activation_time_1={} cumulative_time={}",
                                p.activation_type,
                                bits(v.x),
                                bits(v.y),
                                bits(v.z),
                                p.activation_range,
                                bits(p.activation_time[0]),
                                bits(p.activation_time[1]),
                                bits(p.cumulative_time)
                            );
                        }
                        RandEncKind::TradeLane => {
                            let _ = write!(
                                line,
                                " tl_attack_chance={} tl_attack_chance_read_in={} direction={} entry_ring={} \
                                 exit_ring={} target_tradelane={}",
                                p.tl_attack_chance,
                                p.tl_attack_chance_read_in,
                                p.direction,
                                p.entry_ring,
                                p.exit_ring,
                                p.target_tradelane
                            );
                        }
                    }
                }
            }
            out[7].push(line);
        }
        for (key, npc) in &o.npcs {
            out[8].push(format!(
                "mission={name} key={key}{}",
                render_fields(&npc.fields(), strings)
            ));
        }
        for (key, a) in &o.npc_ship_archs {
            out[9].push(format!(
                "mission={name} key={key} nickname={} ship_archetype={} loadout={} pilot={} state_graph={} \
                 npc_classes={} level={}",
                a.nickname,
                a.ship_archetype,
                a.loadout,
                a.pilot,
                a.state_graph,
                a.npc_classes
                    .iter()
                    .map(u32::to_string)
                    .collect::<Vec<_>>()
                    .join(","),
                a.level
            ));
        }
        for (player, p) in &o.players {
            for (n, objective) in p.objectives.iter().enumerate() {
                out[10].push(format!(
                    "mission={name} player={player} n={n}{}",
                    render_objective(objective)
                ));
            }
        }
        for line in dialogs {
            let (table, text) = line.split_at(2);
            out[if table.starts_with('D') { 11 } else { 12 }].push(text.to_owned());
        }
        render_obj_lists(name, o, strings, &mut out);
    }
    out
}

/// The four tables of P3.5a2: each `[ObjList]` and its directives, each
/// `[MetaBehavior]` and its objects, in key order.
fn render_obj_lists(
    name: &str,
    o: &game_logic::content::mission::object::MissionObject,
    strings: &StringCache,
    out: &mut [Vec<String>],
) {
    for (key, list) in &o.obj_lists {
        out[14].push(format!(
            "mission={name} list={key} directives={}",
            list.len()
        ));
        for (index, d) in list.iter().enumerate() {
            out[15].push(format!(
                "mission={name} list={key} index={index} factory={} type={} directive={} name={}{}",
                d.kind.factory(),
                d.kind.type_code(),
                d.kind.key(),
                hex(&d.name),
                render_fields(&d.kind.fields(), strings)
            ));
        }
    }
    for (key, list) in &o.meta_behaviors {
        out[16].push(format!("mission={name} list={key} objects={}", list.len()));
        for (index, m) in list.iter().enumerate() {
            out[17].push(format!(
                "mission={name} list={key} index={index} type={} class={}{}",
                m.type_code(),
                m.key(),
                render_fields(&m.fields(), strings)
            ));
        }
    }
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
    let cut = |s: &str| s.chars().take(600).collect::<String>();
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
fn data_dump_mission_scripts() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the mission scripts data dump was skipped");
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
    let missions = make_missions(&mut cx);
    let rendered = render(&missions, cx.world.strings());
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "mission scripts: {} missions, {} tables, {} lines",
        missions.len(),
        rendered.len(),
        rendered.iter().map(Vec::len).sum::<usize>()
    );
    for line in &rendered[0] {
        let short: String = line.split(' ').take(2).collect::<Vec<_>>().join(" ");
        let _ = writeln!(summary, "  {short}");
    }
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let dir = oracle.join("missions");
    let mut problems = Vec::new();
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
        let path = dir.join(format!("mission_{table}.txt"));
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
    for p in problems.iter().take(40) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-mission-scripts.txt"), &summary);
    assert!(
        problems.is_empty(),
        "mission scripts: {} differences from the oracle",
        problems.len()
    );
}

/// The fourteen missions over `outdir/`.
fn outdir_missions() -> Option<(Vec<Made>, StringCache)> {
    let root = outdir();
    let (mut world, mut logic) = started(&root, &root.join("data"))?;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let missions = make_missions(&mut cx);
    Some((missions, cx.world.strings().clone()))
}

/// Independent counts from a mission file's text: the sections by header
/// (case-insensitive), the triggers (each `[Trigger]` nickname once), the
/// trigger sections with a `Cnd_*` line, the `Act_*` lines of the counted
/// triggers, and the `rand()` draws (`Cnd_Timer` with a range, a random
/// encounter's `time` activation and `num_forms`).
#[derive(Debug, Default)]
struct TextCounts {
    sections: BTreeMap<String, usize>,
    triggers: usize,
    conditions: usize,
    actions: usize,
    draws: i64,
    ship_lines: usize,
}

fn count_text(text: &str) -> TextCounts {
    let mut counts = TextCounts::default();
    let mut header = String::new();
    let mut seen = std::collections::BTreeSet::new();
    let mut in_trigger = false;
    let mut has_condition = false;
    let mut activation_read = false;
    let mut after_type = false;
    // The [MsnShip] / [MsnFormation] records still listed (their last
    // nickname) and the `ship` lines of each formation, resolved at the end.
    let mut records: Vec<(String, Vec<String>)> = Vec::new();
    let flush = |counts: &mut TextCounts, in_trigger: bool, has_condition: bool| {
        if in_trigger && has_condition {
            counts.conditions += 1;
        }
    };
    for raw in text.lines() {
        let line = raw.split(';').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        if line.starts_with('[') {
            flush(&mut counts, in_trigger, has_condition);
            header = line
                .trim_matches(|c| c == '[' || c == ']')
                .to_ascii_lowercase();
            *counts.sections.entry(header.clone()).or_default() += 1;
            if header == "msnship" || header == "msnformation" {
                records.push((String::new(), Vec::new()));
            }
            in_trigger = false;
            has_condition = false;
            activation_read = false;
            after_type = false;
            continue;
        }
        let (key, value) = line.split_once('=').unwrap_or((line, ""));
        let key = key.trim().to_ascii_lowercase();
        let values: Vec<&str> = value.split(',').map(str::trim).collect();
        if header == "trigger" {
            if key == "nickname" && !in_trigger {
                if seen.insert(values[0].to_ascii_lowercase()) {
                    counts.triggers += 1;
                    in_trigger = true;
                }
                continue;
            }
            if in_trigger && key.starts_with("cnd_") {
                has_condition = true;
                if key == "cnd_timer" && values.len() > 1 && !values[1].is_empty() {
                    counts.draws += 1;
                }
            }
            if in_trigger && key.starts_with("act_") {
                counts.actions += 1;
            }
        }
        if header == "msnrandenc" {
            if key == "encounter_type" {
                after_type = true;
            } else if after_type && key == "activation_type" {
                if values[0].eq_ignore_ascii_case("time") && !activation_read {
                    counts.draws += 1;
                }
                activation_read = true;
            } else if after_type && key == "num_forms" {
                counts.draws += 1;
            }
        }
        if header == "msnship" || header == "msnformation" {
            let record = records.last_mut().expect("a record");
            if key == "nickname" {
                record.0 = value.trim().to_owned();
            } else if key == "ship" {
                record.1.push(values[0].to_owned());
            }
        }
    }
    flush(&mut counts, in_trigger, has_condition);
    // A `ship` line takes the first earlier record of that name out of the
    // list.
    let mut alive: Vec<usize> = Vec::new();
    for (index, (_, ships)) in records.iter().enumerate() {
        alive.push(index);
        for ship in ships {
            if let Some(at) = alive
                .iter()
                .position(|&r| r != index && records[r].0 == *ship)
            {
                alive.remove(at);
                counts.ship_lines += 1;
            }
        }
    }
    counts
}

fn mission_text(file: &[u8]) -> String {
    let source = Host::new(outdir().join("data"));
    String::from_utf8_lossy(&source.read(file).unwrap_or_default()).into_owned()
}

#[test]
fn mission_scripts_on_outdir() {
    let Some((missions, _)) = outdir_missions() else {
        println!("outdir/ is missing: no mission scripts");
        return;
    };
    let mut total_triggers = 0;
    let mut total_actions = 0;
    for ((name, chapter), (s, draws, dialogs)) in CHAPTERS.iter().zip(&missions) {
        let text = mission_text(chapter.file);
        let counts = count_text(&text);
        let section = |h: &str| counts.sections.get(h).copied().unwrap_or(0);
        let o = &s.object;
        let actions: usize = s.triggers.values().map(|t| t.actions.len()).sum();
        let conditions = s
            .triggers
            .values()
            .filter(|t| t.condition.is_some())
            .count();
        let slots: usize = o.ships.iter().map(|g| g.ships.len()).sum();
        let objectives: usize = o.players.values().map(|p| p.objectives.len()).sum();
        let dialog_count = dialogs.iter().filter(|l| l.starts_with('D')).count();
        println!(
            "{name}: {} triggers ({conditions} with a condition), {actions} actions, {} ship groups ({slots} slots), \
             {} solars, {} loot, {} random encounters, {} NPCs, {objectives} objectives, {dialog_count} dialogs, \
             {} dialog costumes, {draws} draws",
            s.triggers.len(),
            o.ships.len(),
            o.solars.len(),
            o.loot.len(),
            o.rand_encs.len(),
            o.npcs.len(),
            s.dialog_costumes.len()
        );
        assert_eq!(s.triggers.len(), counts.triggers, "{name} triggers");
        assert_eq!(conditions, counts.conditions, "{name} conditions");
        assert_eq!(actions, counts.actions, "{name} actions");
        assert_eq!(*draws, counts.draws, "{name} draws");
        assert_eq!(o.solars.len(), section("msnsolar"), "{name} solars");
        assert_eq!(o.loot.len(), section("msnloot"), "{name} loot");
        assert_eq!(
            o.rand_encs.len(),
            section("msnrandenc"),
            "{name} random encounters"
        );
        assert_eq!(objectives, section("nnobjective"), "{name} objectives");
        assert_eq!(slots, section("msnship"), "{name} ship slots");
        assert_eq!(
            o.ships.len(),
            section("msnship") + section("msnformation") - counts.ship_lines,
            "{name} ship groups"
        );
        assert!(o.npcs.len() <= section("npc"), "{name} NPCs");
        assert!(dialog_count <= section("dialog"), "{name} dialogs");
        // Every trigger listens to its condition's ids.
        for t in s.triggers.values() {
            assert_eq!(
                t.condition.is_some(),
                !t.events.is_empty(),
                "{name} trigger {}",
                t.id
            );
        }
        total_triggers += s.triggers.len();
        total_actions += actions;
    }
    // Record 0008, "Mission scripts": 2,984 triggers and 14,203 action lines.
    assert_eq!(total_triggers, 2984);
    assert_eq!(total_actions, 14203);
}

/// The objects of Mission_01a, field by field, against its file's lines.
#[test]
fn mission_01a_field_by_field() {
    let Some((missions, _)) = outdir_missions() else {
        println!("outdir/ is missing: no mission scripts");
        return;
    };
    let (s, _, _) = &missions[0];
    assert_eq!(s.kind, ControllerKind::Mission01a);
    assert_eq!(s.campaign_slot_current, 1);
    assert_eq!(s.campaign_slot_succeed, 2);
    assert_eq!(s.mission_state, 3);
    assert_eq!(s.previous_state, 2);
    assert_eq!(s.field_0x2c0, 0x5528);
    assert!(s.object.player_state);
    assert_eq!(s.object.players.len(), 1);
    let text = mission_text(CHAPTERS[0].1.file);
    // The first trigger of the file, its condition and its actions.
    let first = first_trigger(&text);
    let id = create_id(first.nickname.as_bytes());
    let t = &s.triggers[&id];
    assert_eq!(t.actions.len(), first.actions.len());
    for (action, line) in t.actions.iter().zip(&first.actions) {
        assert!(
            line.to_ascii_lowercase()
                .starts_with(&action.name().to_ascii_lowercase()),
            "{line} against {}",
            action.name()
        );
    }
    if let Some(cnd) = &first.condition {
        let c = t.condition.as_ref().expect("a condition");
        assert!(
            cnd.to_ascii_lowercase()
                .starts_with(&c.name().to_ascii_lowercase())
        );
    }
    // An Act_ActTrig's target is the CreateID of its value.
    for (trigger, actions) in s.triggers.iter().map(|(id, t)| (id, &t.actions)).take(20) {
        for a in actions {
            if let Action::ActTrig(a) = a {
                assert_ne!(a.target_trigger_id, 0, "trigger {trigger}");
            }
        }
    }
    // Each [MsnSolar]'s nickname and position.
    let solars = sections(&text, "msnsolar");
    assert_eq!(solars.len(), s.object.solars.len());
    for (solar, values) in s.object.solars.iter().zip(&solars) {
        assert_eq!(
            String::from_utf8_lossy(&solar.nickname).to_ascii_lowercase(),
            values["nickname"][0].to_ascii_lowercase()
        );
        if let Some(p) = values.get("position") {
            let want: Vec<f32> = p.iter().map(|v| v.parse().unwrap()).collect();
            assert_eq!(
                [solar.position.x, solar.position.y, solar.position.z],
                [want[0], want[1], want[2]]
            );
        }
        assert_eq!(solar.label_ids[0], create_id(&solar.nickname));
    }
    // Each [NPC]'s individual name and affiliation lookup.
    for values in sections(&text, "npc") {
        let key = create_id(values["nickname"].join(", ").as_bytes());
        let npc = &s.object.npcs[&key];
        if let Some(name) = values.get("individual_name") {
            assert_eq!(npc.individual_name, name[0].parse::<i32>().unwrap());
        }
        if values.contains_key("space_costume") {
            assert_ne!(npc.appearance.part_ids[1], 0);
        }
    }
}

/// The first `[Trigger]` of a file: its nickname, its last `Cnd_*` line
/// and its `Act_*` lines.
struct FirstTrigger {
    nickname: String,
    condition: Option<String>,
    actions: Vec<String>,
}

fn first_trigger(text: &str) -> FirstTrigger {
    let mut out = FirstTrigger {
        nickname: String::new(),
        condition: None,
        actions: Vec::new(),
    };
    let mut inside = false;
    for raw in text.lines() {
        let line = raw.split(';').next().unwrap_or("").trim();
        if line.starts_with('[') {
            if inside {
                break;
            }
            inside = line.eq_ignore_ascii_case("[trigger]");
            continue;
        }
        if !inside || line.is_empty() {
            continue;
        }
        let (key, value) = line.split_once('=').unwrap_or((line, ""));
        let key = key.trim();
        if key.eq_ignore_ascii_case("nickname") && out.nickname.is_empty() {
            out.nickname = value.trim().to_owned();
        } else if key.to_ascii_lowercase().starts_with("cnd_") {
            out.condition = Some(line.to_owned());
        } else if key.to_ascii_lowercase().starts_with("act_") {
            out.actions.push(line.to_owned());
        }
    }
    out
}

/// Each section of `header`: its keys' first values, by key.
fn sections(text: &str, header: &str) -> Vec<BTreeMap<String, Vec<String>>> {
    let mut out = Vec::new();
    let mut current: Option<BTreeMap<String, Vec<String>>> = None;
    for raw in text.lines() {
        let line = raw.split(';').next().unwrap_or("").trim();
        if line.starts_with('[') {
            if let Some(c) = current.take() {
                out.push(c);
            }
            let name = line.trim_matches(|c| c == '[' || c == ']');
            if name.eq_ignore_ascii_case(header) {
                current = Some(BTreeMap::new());
            }
            continue;
        }
        if let Some(c) = current.as_mut()
            && let Some((key, value)) = line.split_once('=')
        {
            c.entry(key.trim().to_ascii_lowercase())
                .or_insert_with(|| value.split(',').map(|v| v.trim().to_owned()).collect());
        }
    }
    if let Some(c) = current.take() {
        out.push(c);
    }
    out
}

/// D810 to D815 are not reached on the shipped data: the lines whose C++
/// writes past a bound or through null do not occur in the fourteen files.
#[test]
fn proposed_divergences_are_not_reached_on_outdir() {
    let Some((missions, _)) = outdir_missions() else {
        println!("outdir/ is missing: no mission scripts");
        return;
    };
    let slot_keys = [
        "npc",
        "random_name",
        "label",
        "init_objectives",
        "init_metabehavior",
        "jumper",
        "cargo",
        "hit_points",
        "radius",
    ];
    for ((name, chapter), (_, _, dialogs)) in CHAPTERS.iter().zip(&missions) {
        let text = mission_text(chapter.file);
        let mut header = String::new();
        let mut slot_gone = false;
        // The ship records: nickname, whether `formation` or `ship` dropped
        // their slot, and their `ship` lines.
        let mut records: Vec<(String, bool, Vec<String>)> = Vec::new();
        for raw in text.lines() {
            let line = raw.split(';').next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            if line.starts_with('[') {
                header = line
                    .trim_matches(|c| c == '[' || c == ']')
                    .to_ascii_lowercase();
                slot_gone = false;
                if header == "msnship" || header == "msnformation" {
                    records.push((String::new(), false, Vec::new()));
                }
                continue;
            }
            let (key, value) = line.split_once('=').unwrap_or((line, ""));
            let key = key.trim().to_ascii_lowercase();
            let values: Vec<&str> = value.split(',').map(str::trim).collect();
            let present = values.iter().take_while(|v| !v.is_empty()).count();
            // D810: at most eight accessories.
            if key == "act_sendcomm" {
                assert!(present.saturating_sub(8) <= 8, "{name}: {line}");
            }
            if key == "act_ethercomm" {
                assert!(present.saturating_sub(7) <= 8, "{name}: {line}");
            }
            if header == "npc" && key == "space_costume" {
                assert!(present.saturating_sub(2) <= 8, "{name}: {line}");
            }
            if header == "msnship" || header == "msnformation" {
                let record = records.last_mut().expect("a record");
                if key == "nickname" {
                    // D811: the nickname writes the slot too.
                    assert!(!slot_gone, "{name}: {line}");
                    record.0 = value.trim().to_owned();
                } else if key == "formation" || key == "ship" {
                    slot_gone = true;
                    record.1 = true;
                    if key == "ship" {
                        record.2.push(values[0].to_owned());
                    }
                } else {
                    // D811: no slot key after `formation` or `ship`.
                    assert!(
                        !(slot_gone && slot_keys.contains(&key.as_str())),
                        "{name}: {line}"
                    );
                }
            }
            // D813: every type a name or 0 to 8.
            if header == "nnobjective"
                && key == "type"
                && let Ok(n) = values[0].parse::<i32>()
            {
                assert!((0..=8).contains(&n), "{name}: {line}");
            }
            // D815: every encounter type known.
            if header == "msnrandenc" && key == "encounter_type" {
                assert!(
                    ["sp_attack", "tl_attack"]
                        .iter()
                        .any(|n| values[0].eq_ignore_ascii_case(n)),
                    "{name}: {line}"
                );
            }
        }
        // D812: a resolved `ship` never names its own record or one whose
        // slot is gone.
        let mut alive: Vec<usize> = Vec::new();
        for (index, (own, _, ships)) in records.iter().enumerate() {
            alive.push(index);
            for ship in ships {
                assert_ne!(ship, own, "{name}: {own} names itself");
                if let Some(at) = alive.iter().position(|&r| records[r].0 == *ship) {
                    assert!(!records[alive[at]].1, "{name}: {ship} has no slot");
                    alive.remove(at);
                }
            }
        }
        // D814: every dialog that has a `system` line found its record.
        let without_system = dialogs
            .iter()
            .filter(|l| l.starts_with('D') && l.contains(" system=0 "))
            .count();
        let sections_without = sections(&text, "dialog")
            .iter()
            .filter(|v| !v.contains_key("system"))
            .count();
        assert!(without_system <= sections_without, "{name}: dialog systems");
    }
}

/// Each `[ObjList]` or `[MetaBehavior]` of a file, read independently: its
/// last nickname (lower case) and its element keys (lower case), the first
/// section of a nickname kept.
fn list_sections(text: &str, header: &str, keys: &[&str]) -> BTreeMap<String, Vec<String>> {
    let mut out = BTreeMap::new();
    let mut current: Option<(String, Vec<String>)> = None;
    let mut finish = |current: &mut Option<(String, Vec<String>)>| {
        if let Some((nickname, list)) = current.take() {
            out.entry(nickname).or_insert(list);
        }
    };
    for raw in text.lines() {
        let line = raw.split(';').next().unwrap_or("").trim();
        if line.starts_with('[') {
            finish(&mut current);
            let name = line.trim_matches(|c| c == '[' || c == ']');
            if name.eq_ignore_ascii_case(header) {
                current = Some((String::new(), Vec::new()));
            }
            continue;
        }
        let Some((nickname, list)) = current.as_mut() else {
            continue;
        };
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim().to_ascii_lowercase();
        if key == "nickname" {
            *nickname = value.trim().to_ascii_lowercase();
        } else if keys.iter().any(|k| k.eq_ignore_ascii_case(&key)) {
            list.push(key);
        }
    }
    finish(&mut current);
    out
}

/// P3.5a2: every mission's `[ObjList]` and `[MetaBehavior]` maps against an
/// independent scan of its file (the lists, the first list of a nickname
/// kept, each element's key in order), then one list and one meta
/// behaviour field by field. D820 is not reached.
#[test]
fn obj_lists_on_outdir() {
    use game_logic::content::mission::meta_behavior::MetaBehavior;
    use game_logic::content::mission::obj_list::{DirectiveKind, KEYS};

    let Some((missions, _)) = outdir_missions() else {
        println!("outdir/ is missing: no mission scripts");
        return;
    };
    let keys: Vec<&str> = KEYS
        .iter()
        .map(|(k, _)| std::str::from_utf8(k).unwrap())
        .collect();
    let mut by_key: BTreeMap<&str, usize> = BTreeMap::new();
    let (mut lists, mut directives, mut metas, mut objects) = (0, 0, 0, 0);
    for ((name, chapter), (s, _, _)) in CHAPTERS.iter().zip(&missions) {
        let text = mission_text(chapter.file);
        let o = &s.object;
        let want = list_sections(&text, "objlist", &keys);
        assert_eq!(o.obj_lists.len(), want.len(), "{name} object lists");
        for (nickname, list) in &want {
            let got = &o.obj_lists[&create_id(nickname.as_bytes())];
            let got: Vec<String> = got
                .iter()
                .map(|d| d.kind.key().to_ascii_lowercase())
                .collect();
            assert_eq!(&got, list, "{name} {nickname}");
        }
        let want = list_sections(&text, "metabehavior", &["mb_gotoguide", "mb_dockguide"]);
        assert_eq!(o.meta_behaviors.len(), want.len(), "{name} meta behaviours");
        for (nickname, list) in &want {
            let got = &o.meta_behaviors[&create_id(nickname.as_bytes())];
            let got: Vec<String> = got.iter().map(|m| m.key().to_ascii_lowercase()).collect();
            assert_eq!(&got, list, "{name} {nickname}");
        }
        let count: usize = o.obj_lists.values().map(Vec::len).sum();
        let meta_count: usize = o.meta_behaviors.values().map(Vec::len).sum();
        println!(
            "{name}: {} object lists ({count} directives), {} meta behaviours ({meta_count} objects)",
            o.obj_lists.len(),
            o.meta_behaviors.len()
        );
        lists += o.obj_lists.len();
        directives += count;
        metas += o.meta_behaviors.len();
        objects += meta_count;
        for d in o.obj_lists.values().flatten() {
            *by_key.entry(d.kind.key()).or_default() += 1;
            // D820: no behaviour name longer than its 0x20-byte buffer holds.
            if let DirectiveKind::WaitForPlayerManeuver(w) = &d.kind {
                assert!(w.behavior_name.len() < 0x20, "{name}: {w:?}");
            }
        }
    }
    println!(
        "{lists} object lists, {directives} directives, {metas} meta behaviours, {objects} objects"
    );
    println!("directives by key: {by_key:?}");

    // Mission_01a's `ol_patrol_path`: Delay = 1, MakeNewFormation =
    // transport2_helion, csvX, then a GotoVec and a GotoSpline.
    let o = &missions[0].0.object;
    let list = &o.obj_lists[&create_id(b"ol_patrol_path")];
    assert_eq!(list.len(), 11);
    assert_eq!(list[0].name, b"Delay = 1");
    let DirectiveKind::Delay(d) = &list[0].kind else {
        panic!("{:?}", list[0]);
    };
    assert_eq!((d.priority, d.delay_seconds), (0, 1.0));
    let DirectiveKind::MakeNewFormation(f) = &list[1].kind else {
        panic!("{:?}", list[1]);
    };
    assert_eq!(f.formation, Some(create_id(b"transport2_helion")));
    assert_eq!(f.member_names, [b"csvX".to_vec()]);
    assert!(!f.add_followers);
    assert_eq!(
        (list[1].kind.factory(), list[1].kind.type_code()),
        (0xf, 0xf)
    );
    let DirectiveKind::GotoVec(g) = &list[2].kind else {
        panic!("{:?}", list[2]);
    };
    assert_eq!(
        list[2].name,
        b"GotoVec = goto_no_cruise, -31476, 150, -28660, 500, true, 30"
    );
    assert_eq!(g.op.kind, 1);
    assert_eq!(
        [g.op.destination.x, g.op.destination.y, g.op.destination.z],
        [-31476.0, 150.0, -28660.0]
    );
    assert_eq!((g.op.range, g.op.field_54), (500.0, 30.0));
    assert_eq!(
        (g.op.field_58, g.op.field_59, g.op.cruise, g.op.no_cruise),
        (1, 1, 0, 1)
    );
    assert_eq!(
        (g.op.field_5c, g.op.field_60, g.op.field_64, g.op.field_68),
        (0, 200.0, 500.0, 0)
    );
    assert_eq!((g.field_0x17c, g.priority), (1, 0));
    assert!(g.target_name.is_empty() && g.object_name.is_empty());
    let DirectiveKind::GotoSpline(g) = &list[3].kind else {
        panic!("{:?}", list[3]);
    };
    let words: Vec<f32> = g.op.words_20.iter().map(|&w| f32::from_bits(w)).collect();
    assert_eq!(
        words,
        [
            -31476.0, 150.0, -28660.0, -33374.0, 150.0, -27019.0, -30768.0, 150.0, -29367.0,
            -35769.0, 150.0, -26476.0
        ]
    );
    assert_eq!((g.op.kind, g.op.range, g.op.field_54), (2, 500.0, 30.0));
    assert_eq!((g.op.field_58, g.op.no_cruise, g.field_0x17c), (1, 1, 1));

    // Mission_02's `mb_he01`: MB_GotoGuide = Player, 25, 20, -100, -11994,
    // 0, -80438, 500.
    let list = &missions[2].0.object.meta_behaviors[&create_id(b"mb_he01")];
    let [MetaBehavior::GotoGuide(g)] = list.as_slice() else {
        panic!("{list:?}");
    };
    assert_eq!(g.target_name, b"Player");
    assert_eq!(
        [g.first_position.x, g.first_position.y, g.first_position.z],
        [25.0, 20.0, -100.0]
    );
    assert_eq!(
        [g.destination.x, g.destination.y, g.destination.z],
        [-11994.0, 0.0, -80438.0]
    );
    assert_eq!((g.range, g.priority, g.field_0x176), (500.0, 0, 1));
}
