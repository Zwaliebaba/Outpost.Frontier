//! The random missions' tables, vignette tree and offers (work package
//! P3.7a, decision record 0008 point 8, Gate G3), rendered as `Content.dll`'s
//! `OracleDumpRandomMissions` writes them
//! (`CppSource/Content/oracle_random_missions.cpp`) for
//! `CppSource/Oracle/random_missions.txt`.
//!
//! The port starts a multiplayer server as the oracle does (Content's
//! GameSupport, SpaceSupport and BaseSupport, whose first creation loads
//! the generator object's tables and tree, then `Server_LoadWorldState`).
//! The offers are rebuilt from the oracle's cases: each case's mission
//! values (its objective zone, enemy group and seed, which the C++'s
//! `CreateMission` picked) are the input, and the port runs the offer half
//! of `FinalizeMission` on them (the world's generator seeded with the
//! mission's seed, then `BuildOffer`), after releasing the previous case's
//! offer as the C++'s next `CreateMission` does. The result, the tree path,
//! the draws, the ship group serial, the enemy's name reservations and
//! every offer line are then the port's own.
//!
//! `data_dump_random_missions` reads `OUTPOST_DATA_DIR` (the dump root with
//! `EXE` and `DATA`) and `OUTPOST_ORACLE_DIR` (the oracle's output, with the
//! session's files in `random_missions/`); without either it prints a notice
//! and passes. The other tests check the shipped data in `outdir/` and return
//! early when it is missing. Nothing these tests read or write is committed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::players::PlayerState;
use game_logic::content::random_mission::generator::{self, OfferQuery};
use game_logic::content::random_mission::offer::{
    LootSpawn, Objective, Offer, ShipGroup, release_offer,
};
use game_logic::content::random_mission::vignette::ParamText;
use game_logic::content::random_mission::{RmData, register_dest_system};
use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

/// The tables in the oracle's order, each `rm_<name>.txt`.
const TABLES: [&str; 17] = [
    "npc_rank_to_difficulty",
    "diff2money",
    "bonus_loot",
    "solar_formations",
    "killable_solars",
    "critical_loot",
    "generator",
    "nodes",
    "offer_texts",
    "comm_sequences",
    "missions",
    "offers",
    "offer_ship_groups",
    "offer_ships",
    "offer_solars",
    "offer_objectives",
    "offer_comms",
];

/// `OracleRmPlayerId`.
const PLAYER: u32 = 7;

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn join<T: ToString>(values: &[T], separator: &str) -> String {
    values
        .iter()
        .map(ToString::to_string)
        .collect::<Vec<_>>()
        .join(separator)
}

fn vector(v: Vector) -> String {
    format!("{},{},{}", bits(v.x), bits(v.y), bits(v.z))
}

fn matrix(m: &Matrix) -> String {
    m.m.iter()
        .flatten()
        .map(|&v| bits(v))
        .collect::<Vec<_>>()
        .join(",")
}

fn fmt_str(s: &FmtStr) -> String {
    hex(&s.flatten().unwrap_or_default())
}

fn param_text(t: &ParamText) -> String {
    format!("{}:{}", t.str_id, join(&t.params, "+"))
}

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
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

/// The ten table files, in [`TABLES`]' order.
fn render_tables(rm: &RmData) -> Vec<Vec<String>> {
    let t = &rm.tables;
    let ranks = t
        .npc_rank_to_difficulty
        .iter()
        .map(|(rank, d)| {
            format!(
                "rank={rank} difficulties={}",
                d.iter().map(|&v| bits(v)).collect::<Vec<_>>().join(",")
            )
        })
        .collect();
    let money = t
        .diff2money
        .iter()
        .map(|&(d, m)| format!("difficulty={} money={m}", bits(d)))
        .collect();
    let mut bonus = vec![format!("drop_chance={}", bits(t.bonus_loot_drop_chance))];
    for (faction, records) in &t.bonus_loot {
        for (index, e) in records.iter().enumerate() {
            bonus.push(format!(
                "faction={faction} index={index} archetype={} num_to_drop={},{} difficulty={},{} weight={}",
                e.archetype,
                e.num_to_drop_min,
                e.num_to_drop_max,
                bits(e.difficulty_min),
                bits(e.difficulty_max),
                bits(e.weight)
            ));
        }
    }
    let formations = t
        .solar_formations
        .iter()
        .map(|(key, f)| {
            let slots = f
                .slots
                .iter()
                .map(|s| {
                    format!(
                        "{}/{}/{}/{}",
                        u32::from(s.mandatory),
                        s.solar_type,
                        vector(s.position),
                        matrix(&s.orientation)
                    )
                })
                .collect::<Vec<_>>()
                .join(";");
            format!(
                "formation={key} nickname={} type={} slots={slots}",
                f.nickname, f.kind
            )
        })
        .collect();
    let killable = t
        .killable_solars
        .iter()
        .map(|(key, k)| {
            format!(
                "solar={key} nickname={} type={} string_id={} any={} factions={} archetype={} loadout={} \
                 hitpoints={},{} difficulty={},{} zone_mask={} pilot_choices={}",
                k.nickname,
                k.solar_type,
                k.string_id,
                u32::from(k.any_faction),
                join(&k.factions, ","),
                k.archetype,
                k.loadout,
                bits(k.hitpoints_min),
                bits(k.hitpoints_max),
                bits(k.difficulty_min),
                bits(k.difficulty_max),
                k.zone_type_mask,
                k.pilot_choices
            )
        })
        .collect();
    let critical = t
        .critical_loot
        .iter()
        .enumerate()
        .map(|(index, c)| {
            format!(
                "loot={index} why={},{} name={} archetype={} types={} any={} factions={}",
                c.why_desired[0],
                c.why_desired[1],
                c.name,
                c.archetype,
                join(&c.types, ","),
                u32::from(c.any_faction),
                join(&c.factions, ",")
            )
        })
        .collect();
    let tree = &rm.tree;
    let generator = vec![format!(
        "loaded={} linked={} root={} nodes={} leaves={}",
        u32::from(tree.loaded),
        u32::from(tree.linked),
        tree.root,
        tree.nodes.len(),
        join(&tree.leaves, ",")
    )];
    let mut nodes = Vec::new();
    let mut offer_texts = Vec::new();
    let mut comm_sequences = Vec::new();
    for (key, n) in &tree.nodes {
        let p = &n.params;
        let windows = n
            .windows
            .iter()
            .map(|w| format!("{}:{}", bits(w.min), bits(w.max)))
            .collect::<Vec<_>>()
            .join(",");
        let objectives = p
            .objective_texts
            .iter()
            .map(|(k, o)| format!("{k}/{}:{}", o.str_id, join(&o.params, "+")))
            .collect::<Vec<_>>()
            .join(";");
        nodes.push(format!(
            "node={key} node_id={} nickname={} parents={} children={} implemented={} first_child={} \
             weight={} difficulty={},{} windows={windows} offer_groups={} hostile_groups={} zone_types={} \
             failure={} reward={} objectives={objectives} offer_texts={} comm_sequences={}",
            n.node_id,
            n.nickname,
            join(&n.parents, ","),
            join(&n.children, ","),
            u32::from(n.implemented),
            bits(n.first_child_chance),
            bits(p.weight),
            bits(p.difficulty.min),
            bits(p.difficulty.max),
            join(&p.offer_groups, ","),
            join(&p.hostile_groups, ","),
            p.allowable_zone_types,
            param_text(&p.failure_text),
            param_text(&p.reward_text),
            p.offer_texts.len(),
            p.comm_sequences.len()
        ));
        for (index, text) in p.offer_texts.iter().enumerate() {
            let entries = text
                .entries
                .iter()
                .map(|e| format!("{}/{}/{}", e.str_id, e.form, join(&e.params, "+")))
                .collect::<Vec<_>>()
                .join(";");
            offer_texts.push(format!(
                "node={key} index={index} append={} entries={entries}",
                u32::from(text.append)
            ));
        }
        for (index, s) in p.comm_sequences.iter().enumerate() {
            let lines = s
                .lines
                .iter()
                .map(|l| format!("{}/{}", l.speaker, hex(&l.text)))
                .collect::<Vec<_>>()
                .join(";");
            comm_sequences.push(format!(
                "node={key} index={index} trigger={} target={} f08={} delay={},{} lines={lines}",
                s.trigger,
                s.target,
                bits(s.field_0x08),
                bits(s.min_delay),
                bits(s.max_delay)
            ));
        }
    }
    vec![
        ranks,
        money,
        bonus,
        formations,
        killable,
        critical,
        generator,
        nodes,
        offer_texts,
        comm_sequences,
    ]
}

/// One case of `rm_missions.txt`: the oracle's line, its fields by key.
struct Case {
    fields: BTreeMap<String, String>,
}

impl Case {
    fn parse(line: &str) -> Self {
        let fields = line
            .split(' ')
            .filter_map(|f| f.split_once('='))
            .map(|(k, v)| (k.to_owned(), v.to_owned()))
            .collect();
        Self { fields }
    }

    fn get(&self, key: &str) -> &str {
        self.fields.get(key).map_or("", String::as_str)
    }

    fn u32(&self, key: &str) -> u32 {
        self.get(key).parse().unwrap_or(0)
    }

    fn i32(&self, key: &str) -> i32 {
        self.get(key).parse().unwrap_or(0)
    }

    fn f32(&self, key: &str) -> f32 {
        f32::from_bits(u32::from_str_radix(self.get(key), 16).unwrap_or(0))
    }

    fn vector(&self, key: &str) -> Vector {
        let parts: Vec<f32> = self
            .get(key)
            .split(',')
            .map(|p| f32::from_bits(u32::from_str_radix(p, 16).unwrap_or(0)))
            .collect();
        Vector::new(
            parts.first().copied().unwrap_or(0.0),
            parts.get(1).copied().unwrap_or(0.0),
            parts.get(2).copied().unwrap_or(0.0),
        )
    }

    /// `FinalizeMission`'s query from the mission's values.
    fn query(&self) -> OfferQuery {
        OfferQuery {
            offer_group: self.i32("offer_group"),
            enemy_group: self.u32("enemy_group"),
            reward: self.i32("reward"),
            rep_reward: self.f32("rep_reward"),
            rep_hit: self.f32("rep_hit"),
            offer_base: self.u32("offer_base"),
            difficulty: self.f32("difficulty"),
            objective_zone: self.u32("zone"),
            objective_pos: self.vector("pos"),
            player: self.u32("player"),
            dest_sys: self.u32("dest_sys"),
        }
    }
}

/// The draws from `srand(seed)` to `after` (at most 0x10000; -1 beyond).
fn draws(seed: u32, after: &CrtRand) -> i32 {
    let mut probe = CrtRand::with_seed(seed);
    for n in 0..=0x10000 {
        if probe == *after {
            return n;
        }
        probe.rand();
    }
    -1
}

/// The seven case files, in [`TABLES`]' order from `missions`.
#[derive(Default)]
struct Rendered {
    missions: Vec<String>,
    offers: Vec<String>,
    ship_groups: Vec<String>,
    ships: Vec<String>,
    solars: Vec<String>,
    objectives: Vec<String>,
    comms: Vec<String>,
}

fn loot(cx: &Cx<'_>, list: &[LootSpawn]) -> String {
    list.iter()
        .map(|l| {
            let name = l
                .critical
                .and_then(|i| cx.logic.content.rm.tables.critical_loot.get(i))
                .map_or(0, |r| r.name);
            format!(
                "{}/{}/{name}/{}/{}/{}/{}/{}/{}/{}",
                l.kind,
                l.field_0x100,
                l.archetype,
                l.units,
                l.system,
                bits(l.hit_pts_scale),
                l.can_ai_tractor,
                l.rel_pos,
                vector(l.rel_pos_offset)
            )
        })
        .collect::<Vec<_>>()
        .join(";")
}

fn render_groups(
    cx: &Cx<'_>,
    out: &mut Rendered,
    case: u32,
    component: &str,
    groups: &[ShipGroup],
) {
    for (g, group) in groups.iter().enumerate() {
        out.ship_groups.push(format!(
            "case={case} component={component} group={g} f00={} f01={} f02={} f04={} f08={} system={} \
             position={} f40={} formation={} serial={} ships={}",
            group.field_0x00,
            group.field_0x01,
            group.field_0x02,
            group.field_0x04,
            group.field_0x08,
            group.system,
            vector(group.position),
            bits(group.field_0x40),
            group.formation,
            group.serial,
            group.ships.len()
        ));
        for (s, ship) in group.ships.iter().enumerate() {
            out.ships.push(format!(
                "case={case} component={component} group={g} ship={s} nickname={} random_name={} \
                 affiliation={} npc_ship_arch={} target_name={} d8={} dc={} loot={}",
                hex(&ship.nickname),
                ship.random_name,
                ship.affiliation,
                ship.npc_ship_arch,
                u32::from(ship.target_name),
                ship.field_0xd8,
                ship.field_0xdc,
                loot(cx, &ship.loot)
            ));
        }
    }
}

fn render_objectives(
    out: &mut Rendered,
    case: u32,
    component: &str,
    objectives: &BTreeMap<i32, Objective>,
) {
    for (key, o) in objectives {
        out.objectives.push(format!(
            "case={case} component={component} key={key} type={} str_id={} params={} marker={}:{}:{}",
            o.kind,
            o.text.str_id,
            join(&o.text.params, ","),
            o.marker_system,
            o.marker_value,
            vector(o.marker_position)
        ));
    }
}

fn render_offer(cx: &Cx<'_>, out: &mut Rendered, case: u32, o: &Offer) {
    out.offers.push(format!(
        "case={case} machine={} type={} system={} offer_group={} hostile_group={} difficulty={} reward={} \
         rep_hit={} rep_reward={} offer_base={} mc_costume={} voice={} text={} reward_text={} \
         failure_text={} target_first={} target_second={} target_gender={} target_affiliation={} \
         general={} target_ships={}:{}:{}:{} solars={}:{} ships={}:{}",
        o.machine_type,
        o.kind,
        o.system,
        o.offer_group,
        o.hostile_group,
        bits(o.difficulty),
        o.reward,
        bits(o.rep_hit),
        bits(o.rep_reward),
        o.offer_base,
        o.mc_costume,
        o.voice,
        fmt_str(&o.text),
        param_text(&o.reward_text),
        param_text(&o.failure_text),
        fmt_str(&o.target_name.first),
        fmt_str(&o.target_name.second),
        o.target_name.gender,
        o.target_name.affiliation,
        u32::from(o.general.active),
        u32::from(o.target_ships.component.active),
        vector(o.target_ships.position),
        o.target_ships.field_0x50,
        o.target_ships.field_0x54,
        u32::from(o.solars.component.active),
        vector(o.solars.position),
        u32::from(o.ships.component.active),
        vector(o.ships.position)
    ));
    render_groups(cx, out, case, "target_ships", &o.target_ships.groups);
    render_groups(cx, out, case, "ships", &o.ships.groups);
    for (index, s) in o.solars.solars.iter().enumerate() {
        out.solars.push(format!(
            "case={case} solar={index} nickname={} faction={} orientation={} position={} archetype={} \
             loadout={} pilot={} f4c8={} arch={} type={} dest_sys={} mode={} invincible={} f4e0={} \
             hitpoints={} loot={}",
            hex(&s.nickname),
            s.faction,
            matrix(&s.orientation),
            vector(s.position),
            s.archetype,
            s.loadout,
            s.has_pilot,
            bits(s.field_0x4c8),
            s.arch,
            s.solar_type,
            s.dest_sys,
            s.mode_flag,
            bits(s.invincible_duration),
            s.field_0x4e0,
            s.hitpoints,
            loot(cx, &s.loot)
        ));
    }
    render_objectives(out, case, "general", &o.general.objectives);
    render_objectives(
        out,
        case,
        "target_ships",
        &o.target_ships.component.objectives,
    );
    render_objectives(out, case, "solars", &o.solars.component.objectives);
    render_objectives(out, case, "ships", &o.ships.component.objectives);
    for (index, c) in o.general.conversations.iter().enumerate() {
        let lines = c
            .lines
            .iter()
            .map(|l| format!("{}/{}/{}", l.speaker, l.target, hex(&l.text)))
            .collect::<Vec<_>>()
            .join(";");
        out.comms.push(format!(
            "case={case} conversation={index} trigger={} delay={} ready={} lines={lines}",
            c.trigger,
            bits(c.delay),
            u32::from(c.ready)
        ));
    }
}

/// `" enemy_uses=<pilot names>,<designations>"`.
fn name_uses(cx: &Cx<'_>, group: u32) -> String {
    if cx.logic.content.tables.faction_names(group).is_none() {
        return "-1,-1".to_owned();
    }
    let uses = cx.logic.content.state.faction_name_uses.get(&group);
    format!(
        "{},{}",
        uses.map_or(0, |u| u.pilot.len()),
        uses.map_or(0, |u| u.formation_desig.len())
    )
}

/// The oracle's cases rebuilt (see the module documentation).
fn render_cases(cx: &mut Cx<'_>, oracle: &str) -> Rendered {
    let mut out = Rendered::default();
    let state = PlayerState::new(cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let mut kept: Option<Offer> = None;
    for line in oracle.lines() {
        let case = Case::parse(line);
        let index = case.u32("case");
        if let Some(previous) = kept.take() {
            release_offer(cx, &previous);
        }
        let oracle_result = case.i32("result");
        let query = case.query();
        let (result, path, offers, draw_count) = if oracle_result == 0 || oracle_result == 2 {
            let seed = case.u32("random_seed");
            let rebuilt = generator::rebuild_offer(cx, &query, seed);
            let after = cx.world.rng().clone();
            let count = draws(seed, &after);
            let path = rebuilt.path.text();
            let offers = u32::from(rebuilt.offer.is_some());
            kept = rebuilt.offer;
            (rebuilt.result, path, offers, count)
        } else {
            (oracle_result, String::new(), 0, -1)
        };
        out.missions.push(format!(
            "case={index} base={} system={} group={} difficulty={} seed={} result={result} random_seed={} \
             player={} offer_base={} offer_group={} enemy_group={} dest_sys={} zone={} pos={} reward={} \
             rep_reward={} rep_hit={} path={path} offers={offers} draws={draw_count} serial={} enemy_uses={}",
            case.u32("base"),
            case.u32("system"),
            case.u32("group"),
            case.get("difficulty"),
            case.u32("seed"),
            case.u32("random_seed"),
            query.player,
            query.offer_base,
            query.offer_group,
            query.enemy_group,
            query.dest_sys,
            query.objective_zone,
            vector(query.objective_pos),
            query.reward,
            bits(query.rep_reward),
            bits(query.rep_hit),
            cx.logic.content.state.rm.ship_group_serial,
            name_uses(cx, query.enemy_group)
        ));
        if let Some(offer) = &kept {
            render_offer(cx, &mut out, index, offer);
        }
    }
    if let Some(previous) = kept.take() {
        release_offer(cx, &previous);
    }
    cx.logic.content.state.players.remove(&PLAYER);
    out
}

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
fn data_dump_random_missions() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the random missions data dump was skipped");
        return;
    };
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_ORACLE_DIR is not set; the random missions data dump was skipped"
        );
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
    let dir = oracle.join("random_missions");
    let read = |name: &str| std::fs::read(dir.join(format!("rm_{name}.txt")));
    let rm = cx.logic.content.rm.clone();
    let mut rendered = render_tables(&rm);
    let missions_text = read("missions")
        .map(|b| String::from_utf8_lossy(&b).into_owned())
        .unwrap_or_default();
    let cases = render_cases(&mut cx, &missions_text);
    rendered.extend([
        cases.missions,
        cases.offers,
        cases.ship_groups,
        cases.ships,
        cases.solars,
        cases.objectives,
        cases.comms,
    ]);
    let mut summary = String::new();
    let offers = rendered[11].len();
    let built = rendered[10]
        .iter()
        .filter(|l| l.contains(" result=0 "))
        .count();
    let _ = writeln!(
        summary,
        "random missions: {} nodes, {} cases, {built} with an offer, {offers} offers, {} tables, {} lines",
        rm.tree.nodes.len(),
        rendered[10].len(),
        rendered.len(),
        rendered.iter().map(Vec::len).sum::<usize>()
    );
    let mut problems = Vec::new();
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
        match read(table) {
            Ok(bytes) => problems.extend(compare(table, &String::from_utf8_lossy(&bytes), lines)),
            Err(e) => problems.push(format!("rm_{table}.txt: {e}")),
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
    let _ = std::fs::write(oracle.join("rust-random-missions.txt"), &summary);
    assert!(
        problems.is_empty(),
        "random missions: {} differences from the oracle",
        problems.len()
    );
}

/// The shipped data's server, or `None` when `outdir/` is missing.
fn outdir_started() -> Option<(World, Logic)> {
    let root = outdir();
    started(&root, &root.join("data"))
}

/// Independent counts from an INI file's text: the sections by header,
/// without regard to case, and the lines of a key.
fn count_text(path: &Path, header: &str, key: &str) -> (usize, usize) {
    let text = std::fs::read(path).unwrap_or_default();
    let text = String::from_utf8_lossy(&text);
    let mut headers = 0;
    let mut keys = 0;
    for line in text.lines() {
        let line = line.split(';').next().unwrap_or("").trim();
        if line.eq_ignore_ascii_case(&format!("[{header}]")) {
            headers += 1;
        }
        if let Some((k, _)) = line.split_once('=')
            && k.trim().eq_ignore_ascii_case(key)
        {
            keys += 1;
        }
    }
    (headers, keys)
}

#[test]
fn the_generator_loads_the_shipped_tables() {
    let Some((world, logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let rm = &logic.content.rm;
    let dir = outdir().join("data/RANDOMMISSIONS");
    let (_, ranks) = count_text(&dir.join("npcranktodiff.ini"), "x", "NPCRank");
    assert_eq!(rm.tables.npc_rank_to_difficulty.len(), ranks);
    let (_, money) = count_text(&dir.join("diff2money.ini"), "x", "Diff2Money");
    assert_eq!(rm.tables.diff2money.len(), money);
    assert!(rm.tables.diff2money.windows(2).all(|w| w[0].0 < w[1].0));
    let (critical, _) = count_text(&dir.join("vignettecriticalloot.ini"), "critical_loot", "x");
    assert_eq!(rm.tables.critical_loot.len(), critical);
    let (killable, _) = count_text(&dir.join("killablesolars.ini"), "KillableSolar", "x");
    assert!(rm.tables.killable_solars.len() <= killable);
    assert!(!rm.tables.killable_solars.is_empty());
    // Every kept archetype is a solar, so `GetMaxHitPoints` answers for each
    // and no entry reads the slot another section left (D870).
    for k in rm.tables.killable_solars.values() {
        assert!(
            world.data().archetypes.solars.contains_key(&k.archetype),
            "{}",
            k.nickname
        );
    }
    let (formations, _) = count_text(&dir.join("solarformations.ini"), "SolarFormation", "x");
    assert_eq!(rm.tables.solar_formations.len(), formations);
    assert!(rm.tables.bonus_loot_drop_chance > 0.0 && rm.tables.bonus_loot_drop_chance <= 1.0);
    let params = dir.join("vignetteparams.ini");
    let nodes = ["DecisionNode", "DocumentationNode", "DataNode"]
        .iter()
        .map(|h| count_text(&params, h, "x").0)
        .sum::<usize>();
    // Every child line names a node on the shipped data, so Link adds none.
    assert_eq!(rm.tree.nodes.len(), nodes);
    assert!(rm.tree.loaded && rm.tree.linked);
    // Node 2 is the only node no child line names.
    assert_eq!(rm.tree.root, 2);
    assert!(rm.tree.nodes[&2].parents.is_empty());
    assert!(!rm.tree.leaves.is_empty());
    for leaf in &rm.tree.leaves {
        let n = &rm.tree.nodes[leaf];
        assert!(n.implemented && n.children.is_empty());
        assert!(!n.windows.is_empty());
    }
    // The four unimplemented nodes and everything below them are off.
    for id in [193, 248, 611, 614] {
        assert!(!rm.tree.nodes[&id].implemented, "{id}");
    }
}

/// The enemy group of a random mission populates a zone, and the generator
/// reads its properties and names without a test (D874, D876): every such
/// group has both, with the names an assassination target takes.
#[test]
fn every_populating_group_has_properties_and_names() {
    let Some((world, logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let tables = &logic.content.tables;
    let mut entries = 0;
    for zone in world.data().universe.zones.values() {
        for (group, _) in &zone.population {
            entries += 1;
            assert!(tables.faction_props(*group).is_some(), "{group}");
            let names = tables.faction_names(*group).expect("names");
            if !names.male_first_names.is_empty() {
                assert!(!names.last_names.is_empty(), "{group}");
                assert!(!names.formation_desigs.is_empty(), "{group}");
            }
        }
    }
    assert!(entries > 0);
}

#[test]
fn offers_build_from_a_seed_on_the_shipped_data() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = api::misc::system_id(&mut cx, b"he01").expect("he01");
    register_dest_system(&mut cx, he01);
    let zones: Vec<u32> = cx.logic.content.state.systems[&he01]
        .zones
        .iter()
        .filter(|z| z.lawful)
        .map(|z| z.zone_id)
        .collect();
    assert!(!zones.is_empty(), "he01 has lawful zones");
    let offer_group = api::reputation::reputation_group(&cx, b"he_n_grp").expect("he_n_grp");
    let enemy_group = api::reputation::reputation_group(&cx, b"fc_x_grp").expect("fc_x_grp");
    let base = api::misc::base_id(&mut cx, b"he01_01_Base").expect("he01_01_Base");
    let rm = cx.logic.content.rm.clone();
    let mut built = 0;
    let mut machines = BTreeMap::new();
    for (index, &zone) in zones.iter().enumerate().take(6) {
        let position = api::zone::pos(&mut cx, zone);
        for difficulty in [1.5f32, 8.0, 30.0] {
            for seed in [1u32, 2, 3, 4] {
                let query = OfferQuery {
                    offer_group: offer_group as i32,
                    enemy_group,
                    reward: rm.tables.difficulty_to_money(difficulty) as i32,
                    rep_reward: 0.15,
                    rep_hit: -0.15,
                    offer_base: base,
                    difficulty,
                    objective_zone: zone,
                    objective_pos: position,
                    player: PLAYER,
                    dest_sys: he01,
                };
                let seed = seed * 7919 + 12_345 + index as u32 * 104_729;
                let rebuilt = generator::rebuild_offer(&mut cx, &query, seed);
                if let Some(offer) = &rebuilt.offer {
                    built += 1;
                    assert_eq!(rebuilt.result, 0);
                    assert!(!rebuilt.path.bits.is_empty());
                    assert_eq!(offer.text.mad_lib_id, 0x50001);
                    assert!(!offer.text.values.is_empty());
                    assert!((1..=3).contains(&offer.machine_type));
                    assert_eq!(offer.offer_base, base);
                    *machines.entry(offer.machine_type).or_insert(0) += 1;
                    release_offer(&mut cx, offer);
                }
            }
        }
    }
    println!("offers built: {built}, by machine type: {machines:?}");
    // Every mission type of the tree is reached: destroy solars, and the
    // assassinations of a ship and of a solar.
    for machine in 1..=3 {
        assert!(
            machines.contains_key(&machine),
            "no offer of machine type {machine}"
        );
    }
}

#[test]
fn a_rebuild_from_the_same_seed_repeats_the_path_and_the_draws() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = api::misc::system_id(&mut cx, b"he01").expect("he01");
    register_dest_system(&mut cx, he01);
    let zone = cx.logic.content.state.systems[&he01]
        .zones
        .iter()
        .find(|z| z.lawful)
        .map(|z| z.zone_id)
        .expect("a lawful zone");
    let position = api::zone::pos(&mut cx, zone);
    let offer_group = api::reputation::reputation_group(&cx, b"he_n_grp").expect("he_n_grp");
    let enemy_group = api::reputation::reputation_group(&cx, b"fc_x_grp").expect("fc_x_grp");
    let base = api::misc::base_id(&mut cx, b"he01_01_Base").expect("he01_01_Base");
    let query = OfferQuery {
        offer_group: offer_group as i32,
        enemy_group,
        reward: 9000,
        rep_reward: 0.15,
        rep_hit: -0.15,
        offer_base: base,
        difficulty: 8.0,
        objective_zone: zone,
        objective_pos: position,
        player: PLAYER,
        dest_sys: he01,
    };
    let mut seen = Vec::new();
    for _ in 0..2 {
        let rebuilt = generator::rebuild_offer(&mut cx, &query, 0x1234);
        let after = cx.world.rng().clone();
        seen.push((rebuilt.result, rebuilt.path.clone(), draws(0x1234, &after)));
        if let Some(offer) = &rebuilt.offer {
            release_offer(&mut cx, offer);
        }
    }
    assert_eq!(seen[0], seen[1]);
}

#[test]
fn a_rendered_case_reads_back_as_the_same_case() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = api::misc::system_id(&mut cx, b"he01").expect("he01");
    register_dest_system(&mut cx, he01);
    let zone = cx.logic.content.state.systems[&he01]
        .zones
        .iter()
        .find(|z| z.lawful)
        .map(|z| z.zone_id)
        .expect("a lawful zone");
    let position = api::zone::pos(&mut cx, zone);
    let offer_group = api::reputation::reputation_group(&cx, b"he_n_grp").expect("he_n_grp");
    let enemy_group = api::reputation::reputation_group(&cx, b"fc_x_grp").expect("fc_x_grp");
    let base = api::misc::base_id(&mut cx, b"he01_01_Base").expect("he01_01_Base");
    let mut text = String::new();
    for (case, seed) in [12_345u32, 20_264, 28_183, 36_102, 44_021]
        .iter()
        .enumerate()
    {
        let _ = writeln!(
            text,
            "case={case} base={base} system={he01} group={offer_group} difficulty={} seed=1 \
             result=0 random_seed={seed} player={PLAYER} offer_base={base} offer_group={} \
             enemy_group={enemy_group} dest_sys={he01} zone={zone} pos={} reward=9000 \
             rep_reward={} rep_hit={} path= offers=0 draws=0 serial=0 enemy_uses=0,0",
            bits(8.0),
            offer_group as i32,
            vector(position),
            bits(0.15),
            bits(-0.15)
        );
    }
    let first = render_cases(&mut cx, &text);
    assert!(!first.offers.is_empty(), "an offer was built");
    let (mut world, mut logic) = outdir_started().expect("outdir/");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let second = render_cases(&mut cx, &first.missions.join("\n"));
    assert_eq!(first.missions, second.missions);
    assert_eq!(first.offers, second.offers);
    assert_eq!(first.ship_groups, second.ship_groups);
    assert_eq!(first.ships, second.ships);
    assert_eq!(first.solars, second.solars);
    assert_eq!(first.objectives, second.objectives);
    assert_eq!(first.comms, second.comms);
}
