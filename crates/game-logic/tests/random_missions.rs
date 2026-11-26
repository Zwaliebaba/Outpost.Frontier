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
//! Every line is compared exactly but for the fields that come from
//! `random_inside_ring` (D879): the assassination target's position in the
//! `target_ships` part of an offer line and the `position` of the target
//! component's ship groups. Those are compared within the Tier B bound of
//! the patrol points (4 ulps or 1e-7, `support`), and each line equal only
//! within it is listed in the summary.
//!
//! `data_dump_random_missions` reads `OUTPOST_DATA_DIR` (the dump root with
//! `EXE` and `DATA`) and `OUTPOST_ORACLE_DIR` (the oracle's output, with the
//! session's files in `random_missions/`); without either it prints a notice
//! and passes. The other tests check the shipped data in `outdir/` and return
//! early when it is missing. Nothing these tests read or write is committed.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::Instance;
use game_logic::content::bases::{self, BaseFaction, BaseNpcMission};
use game_logic::content::players::{self, PlayerState};
use game_logic::content::random_mission::generator::{self, OfferQuery};
use game_logic::content::random_mission::mission::{self, Loader, MissionData, MissionRequest};
use game_logic::content::random_mission::offer::{
    LootSpawn, Objective, Offer, ShipGroup, release_offer,
};
use game_logic::content::random_mission::vendor;
use game_logic::content::random_mission::vignette::ParamText;
use game_logic::content::random_mission::{RmData, RmDataKey, register_dest_system};
use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::base_props::MissionVendorProperties;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::msvcrt::text_mode_write;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

mod support;

use support::{POINT_ABSOLUTE, POINT_ULPS, floats_within_bound, within_bound};

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

/// Whether two lines of `table` are equal but for the fields that come from
/// `random_inside_ring`, and those within the Tier B bound (D879): the
/// assassination target's position, `FillTargetShips`' objective position
/// plus the ring offset, which is the offer's `target_ships` position
/// (`active:x,y,z:f50:f54`, its second part) and the `position` of every
/// ship group of the `target_ships` component. Every other field, and every
/// other table, compares exactly.
fn ring_within_bound(table: &str, oracle: &str, rust: &str) -> bool {
    let (w, g): (Vec<&str>, Vec<&str>) = (oracle.split(' ').collect(), rust.split(' ').collect());
    if w.len() != g.len() {
        return false;
    }
    let target_group = w.contains(&"component=target_ships");
    w.iter().zip(&g).all(|(a, b)| {
        if a == b {
            return true;
        }
        match table {
            "offers" => {
                let (Some(x), Some(y)) = (
                    a.strip_prefix("target_ships="),
                    b.strip_prefix("target_ships="),
                ) else {
                    return false;
                };
                let (x, y): (Vec<&str>, Vec<&str>) =
                    (x.split(':').collect(), y.split(':').collect());
                x.len() == 4
                    && y.len() == 4
                    && x[0] == y[0]
                    && x[2..] == y[2..]
                    && floats_within_bound(x[1], y[1])
            }
            "offer_ship_groups" if target_group => {
                match (a.strip_prefix("position="), b.strip_prefix("position=")) {
                    (Some(x), Some(y)) => floats_within_bound(x, y),
                    _ => false,
                }
            }
            _ => false,
        }
    })
}

/// The differences of one table from the oracle's file; a line that
/// differs only in the ring's fields, within the Tier B bound, is listed in
/// `bounded` ([`ring_within_bound`]).
fn compare(
    table: &str,
    oracle: &str,
    rendered: &[String],
    bounded: &mut Vec<String>,
) -> Vec<String> {
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
            if ring_within_bound(table, w, g) {
                bounded.push(format!(
                    "{table} line {} (within the Tier B bound):\n  oracle: {}\n  rust:   {}",
                    i + 1,
                    cut(w),
                    cut(g)
                ));
                continue;
            }
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
    let mut bounded = Vec::new();
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
        match read(table) {
            Ok(bytes) => problems.extend(compare(
                table,
                &String::from_utf8_lossy(&bytes),
                lines,
                &mut bounded,
            )),
            Err(e) => problems.push(format!("rm_{table}.txt: {e}")),
        }
    }
    let _ = writeln!(
        summary,
        "{} tables compared with the oracle, {} differences",
        rendered.len(),
        problems.len()
    );
    let _ = writeln!(
        summary,
        "{} lines equal within the Tier B bound ({POINT_ULPS} ulps or {POINT_ABSOLUTE:e}), the rest exactly",
        bounded.len()
    );
    for b in &bounded {
        let _ = writeln!(summary, "{b}");
    }
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

#[test]
fn the_ring_bound_takes_only_the_target_position() {
    // An offer line, cut to the fields around the target's position, whose
    // y differs by 3 ulps: within the bound, as is a 1e-7 difference near 0.
    let offer = |position: &str, f50: &str, solars: &str| {
        format!(
            "case=4 machine=2 type=2 system=2745 general=1 target_ships=1:{position}:{f50}:1 \
             solars=0:{solars} ships=1:c6a1b8f0,44fa0000,47c35000"
        )
    };
    let oracle = offer(
        "c6a2ce5f,43c80000,47b6a2b1",
        "2",
        "00000000,00000000,00000000",
    );
    let near = offer(
        "c6a2ce5f,43c80003,47b6a2b1",
        "2",
        "00000000,00000000,00000000",
    );
    assert!(ring_within_bound("offers", &oracle, &near));
    let tiny = offer(
        "c6a2ce5f,43c80000,33d6bf94",
        "2",
        "00000000,00000000,00000000",
    );
    let zero = offer(
        "c6a2ce5f,43c80000,00000000",
        "2",
        "00000000,00000000,00000000",
    );
    assert!(ring_within_bound("offers", &tiny, &zero));
    // Five ulps, another field of the component, or another position is not.
    let far = offer(
        "c6a2ce5f,43c80005,47b6a2b1",
        "2",
        "00000000,00000000,00000000",
    );
    assert!(!ring_within_bound("offers", &oracle, &far));
    let other = offer(
        "c6a2ce5f,43c80003,47b6a2b1",
        "3",
        "00000000,00000000,00000000",
    );
    assert!(!ring_within_bound("offers", &oracle, &other));
    let solars = offer(
        "c6a2ce5f,43c80000,47b6a2b1",
        "2",
        "00000000,00000001,00000000",
    );
    assert!(!ring_within_bound("offers", &oracle, &solars));
    // A ship group of the target's component takes the bound in its
    // position; one of the other ship component does not.
    let group = |component: &str, position: &str| {
        format!(
            "case=4 component={component} group=0 f00=1 f01=0 f02=1 f04=0 f08=4294967295 \
             system=2745 position={position} f40=bf666666 formation=1 serial=17 ships=3"
        )
    };
    let oracle = group("target_ships", "c6a2ce5f,43c80000,47b6a2b1");
    let near = group("target_ships", "c6a2ce5b,43c80000,47b6a2b1");
    assert!(ring_within_bound("offer_ship_groups", &oracle, &near));
    let far = group("target_ships", "c6a2ce5a,43c80000,47b6a2b1");
    assert!(!ring_within_bound("offer_ship_groups", &oracle, &far));
    let oracle = group("ships", "c6a2ce5f,43c80000,47b6a2b1");
    let near = group("ships", "c6a2ce5b,43c80000,47b6a2b1");
    assert!(!ring_within_bound("offer_ship_groups", &oracle, &near));
    // No other table takes it, and a bounded line is listed while a
    // bit-equal one counts as exact.
    let solar = "case=4 solar=0 position=c6a2ce5f,43c80000,47b6a2b1";
    let moved = "case=4 solar=0 position=c6a2ce5b,43c80000,47b6a2b1";
    assert!(!ring_within_bound("offer_solars", solar, moved));
    let oracle_file = format!(
        "{}\n{}\n",
        group("target_ships", "c6a2ce5f,43c80000,47b6a2b1"),
        group("ships", "c6a2ce5f,43c80000,47b6a2b1")
    );
    let rendered = vec![
        group("target_ships", "c6a2ce5b,43c80000,47b6a2b1"),
        group("ships", "c6a2ce5f,43c80000,47b6a2b1"),
    ];
    let mut bounded = Vec::new();
    assert!(compare("offer_ship_groups", &oracle_file, &rendered, &mut bounded).is_empty());
    assert_eq!(bounded.len(), 1);
    assert!(bounded[0].starts_with("offer_ship_groups line 1 (within the Tier B bound)"));
    assert!(within_bound(0x3f80_0000, 0x3f80_0004));
    assert!(!within_bound(0x3f80_0000, 0x3f80_0005));
}

// ---------------------------------------------------------------------------
// P3.7c: missions the port creates, and the `[RandomMission]` saves.
// ---------------------------------------------------------------------------

/// `OracleRmBases`. The last three are [`D954_BASES`], at the end so
/// that the counters their cases can move (the ship group serial, the
/// name pools' uses, the offer ids, the save numbers) reach no case of
/// the other bases, which stay exact.
const BASES: [&str; 29] = [
    "he01_01_Base",
    "he01_02_Base",
    "he01_10_Base",
    "he02_01_Base",
    "he03_01_Base",
    "he04_01_Base",
    "ve01_01_Base",
    "ve01_08_Base",
    "ve02_01_Base",
    "ve04_06_Base",
    "ve05_01_Base",
    "ve06_01_Base",
    "or01_03_Base",
    "or02_04_Base",
    "or05_02_Base",
    "dm01_04_Base",
    "dm02_01_Base",
    "dm03_01_Base",
    "dm05_01_Base",
    "fc03_01_Base",
    "fc04_02_Base",
    "fr01_04_Base",
    "fr03_01_Base",
    "fr07_01_Base",
    "oe01_01_Base",
    "pe01_01_Base",
    "fr01_01_Base",
    "he01_15_Base",
    "ve05_02_Base",
];

/// `OracleRmDifficulties`.
const DIFFICULTIES: [f32; 5] = [0.2, 1.5, 8.0, 30.0, 85.0];

/// `OracleRmSeeds`.
const SEEDS: [u32; 2] = [1, 0x4f52_4143];

/// The tables of a load, in `rmload_<table>.txt`'s order.
const LOAD_TABLES: [&str; 7] = [
    "missions",
    "offers",
    "offer_ship_groups",
    "offer_ships",
    "offer_solars",
    "offer_objectives",
    "offer_comms",
];

/// A base of [`BASES`] that Content knows, in a system with a record: its
/// id and system.
fn known_base(cx: &mut Cx<'_>, name: &str) -> Option<(u32, u32)> {
    let base = api::misc::base_id(cx, name.as_bytes()).unwrap_or(0);
    if !cx.logic.content.state.bases.contains_key(&base) {
        return None;
    }
    let system = api::misc::base_system(cx, base).ok()?;
    cx.logic
        .content
        .state
        .systems
        .contains_key(&system)
        .then_some((base, system))
}

/// The `[BaseFaction]`s of a base with a mission type, faction properties
/// and names, in order.
fn mission_factions(cx: &Cx<'_>, base: u32) -> Vec<BaseFaction> {
    let tables = &cx.logic.content.tables;
    cx.logic.content.state.bases[&base]
        .factions
        .iter()
        .filter(|f| {
            !f.mission_types.is_empty()
                && tables.faction_props(f.group).is_some()
                && tables.faction_names(f.group).is_some()
        })
        .cloned()
        .collect()
}

fn mission_of(cx: &Cx<'_>, key: Option<RmDataKey>) -> Option<MissionData> {
    key.and_then(|k| cx.logic.content.state.rm.missions.get(k))
        .cloned()
}

/// The port's missions for the oracle's cases (`DumpCases`): every case is
/// the port's own `CreateMission`, on one loader, with the zones emptied,
/// the player in the base's system and the generator seeded first.
fn render_created(cx: &mut Cx<'_>) -> Rendered {
    let mut out = Rendered::default();
    let state = PlayerState::new(cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let mut loader = Loader::new();
    let mut case = 0u32;
    for name in BASES {
        let Some((base, system)) = known_base(cx, name) else {
            continue;
        };
        for faction in mission_factions(cx, base) {
            for difficulty in DIFFICULTIES {
                for seed in SEEDS {
                    cx.logic.content.state.rm.objective_zones.clear();
                    if let Some(s) = cx.logic.content.state.players.get_mut(&PLAYER) {
                        s.system = Some(system);
                    }
                    cx.world.rng().seed(seed);
                    let request = MissionRequest {
                        player: PLAYER,
                        offer_base: base,
                        offer_group: faction.group as i32,
                        difficulty,
                    };
                    let (result, key) = loader.create_mission(cx, &request);
                    let m = mission_of(cx, Some(key)).expect("the mission");
                    let draws_count = if result == 0 || result == 2 {
                        let after = cx.world.rng().clone();
                        draws(m.random_seed, &after)
                    } else {
                        -1
                    };
                    out.missions.push(format!(
                        "case={case} base={base} system={system} group={} difficulty={} seed={seed} \
                         result={result} random_seed={} player={} offer_base={} offer_group={} \
                         enemy_group={} dest_sys={} zone={} pos={} reward={} rep_reward={} rep_hit={} \
                         path={} offers={} draws={draws_count} serial={} enemy_uses={}",
                        faction.group,
                        bits(difficulty),
                        m.random_seed,
                        m.player,
                        m.offer_base,
                        m.offer_group,
                        m.enemy_group,
                        m.dest_sys,
                        m.objective_zone,
                        vector(m.objective_pos),
                        m.reward,
                        bits(m.rep_reward),
                        bits(m.rep_hit),
                        m.tree_path.text(),
                        m.offers.len(),
                        cx.logic.content.state.rm.ship_group_serial,
                        name_uses(cx, m.enemy_group)
                    ));
                    if let Some(offer) = m.offers.first() {
                        render_offer(cx, &mut out, case, offer);
                    }
                    case += 1;
                }
            }
        }
    }
    loader.release(cx);
    cx.logic.content.state.rm.objective_zones.clear();
    cx.logic.content.state.players.remove(&PLAYER);
    out
}

/// The seven case files of a [`Rendered`], in [`TABLES`]' order from
/// `missions`.
fn case_files(r: Rendered) -> [Vec<String>; 7] {
    [
        r.missions,
        r.offers,
        r.ship_groups,
        r.ships,
        r.solars,
        r.objectives,
        r.comms,
    ]
}

/// Compares `files` (named by `tables`) with `<dir>/<prefix><table>.txt`,
/// adding to the summary; the problems.
fn compare_files(
    summary: &mut String,
    dir: &Path,
    prefix: &str,
    tables: &[&str],
    files: &[Vec<String>],
    bounded: &mut Vec<String>,
) -> Vec<String> {
    let mut problems = Vec::new();
    for (table, lines) in tables.iter().zip(files) {
        let _ = writeln!(summary, "  {prefix}{table}: {} lines", lines.len());
        match std::fs::read(dir.join(format!("{prefix}{table}.txt"))) {
            Ok(bytes) => problems.extend(compare(
                table,
                &String::from_utf8_lossy(&bytes),
                lines,
                bounded,
            )),
            Err(e) => problems.push(format!("{prefix}{table}.txt: {e}")),
        }
    }
    problems
}

/// Ends a summary: the counts, the bounded lines and the first problems,
/// printed and written to `path`.
fn finish_summary(
    summary: &mut String,
    path: &Path,
    problems: &[String],
    bounded: &[String],
    d954: &[String],
) {
    let _ = writeln!(
        summary,
        "{} differences; {} lines equal within the Tier B bound ({POINT_ULPS} ulps or {POINT_ABSOLUTE:e}), the rest exactly",
        problems.len(),
        bounded.len()
    );
    for b in bounded {
        let _ = writeln!(summary, "{b}");
    }
    let _ = writeln!(
        summary,
        "D954: {} cases of the bases without a dock object differ in the dock position's fields only",
        d954.len()
    );
    for c in d954 {
        let _ = writeln!(summary, "  {c}");
    }
    for p in problems.iter().take(40) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(path, summary.as_bytes());
}

// ---------------------------------------------------------------------------
// D954: the bases without a dock object.
// ---------------------------------------------------------------------------

/// The bases that offer missions without an object docking with them on
/// the shipped data (asserted by
/// `every_vendor_faction_has_properties_weights_and_a_dock`), where
/// `CreateMission`'s search starts from the reconstruction's zero instead
/// of the original's unset local (D954).
const D954_BASES: [&str; 3] = ["fr01_01_Base", "he01_15_Base", "ve05_02_Base"];

/// The fields of an `rm_missions.txt` line that derive from the dock
/// position: the objective zone the search finds and everything that
/// follows from it (its system and position, the enemy group picked
/// around it, the result, the offer's tree path and count, the draws, the
/// ship group serial and the enemy's name reservations). The request, the
/// seeds, the reward and the reputation values do not.
const D954_MISSION_FIELDS: [&str; 10] = [
    "result",
    "enemy_group",
    "dest_sys",
    "zone",
    "pos",
    "path",
    "offers",
    "draws",
    "serial",
    "enemy_uses",
];

/// The fields of an `rm_offers.txt` line that do not derive from the dock
/// position: the query's values and the fixed voice.
const D954_OFFER_FIXED: [&str; 8] = [
    "case",
    "offer_group",
    "difficulty",
    "reward",
    "rep_hit",
    "rep_reward",
    "offer_base",
    "voice",
];

/// The keys of a save that do not derive from the dock position. A bar
/// offer tries again on "no offer" with a new difficulty and seed, and a
/// vendor attempt that finds no zone moves the next faction and mission
/// type pick, so the offering group, the difficulty, the reward and the
/// seed derive too.
const D954_SAVE_FIXED: [&str; 5] = [
    "offer_base",
    "offer_ids",
    "jump_pref",
    "rep_reward",
    "rep_hit",
];

/// The base ids of [`D954_BASES`].
fn d954_base_ids(cx: &mut Cx<'_>) -> BTreeSet<u32> {
    D954_BASES
        .iter()
        .filter_map(|name| api::misc::base_id(cx, name.as_bytes()).ok())
        .collect()
}

/// A line's `key=value` fields, in order.
fn fields(line: &str) -> Vec<(&str, &str)> {
    line.split(' ').filter_map(|f| f.split_once('=')).collect()
}

fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    fields(line)
        .into_iter()
        .find(|(k, _)| *k == key)
        .map(|(_, v)| v)
}

/// The cases of `rm_missions.txt` lines (oracle's and port's) whose base
/// is one of `bases`.
fn d954_cases<'a>(lines: impl Iterator<Item = &'a str>, bases: &BTreeSet<u32>) -> BTreeSet<u32> {
    lines
        .filter(|l| {
            field(l, "base")
                .and_then(|b| b.parse().ok())
                .is_some_and(|b| bases.contains(&b))
        })
        .filter_map(|l| field(l, "case").and_then(|c| c.parse().ok()))
        .collect()
}

/// Lines grouped by their `key` value, in order.
fn group_by<'a>(lines: impl Iterator<Item = &'a str>, key: &str) -> BTreeMap<u32, Vec<&'a str>> {
    let mut out: BTreeMap<u32, Vec<&str>> = BTreeMap::new();
    for l in lines {
        let k = field(l, key)
            .and_then(|v| v.parse().ok())
            .unwrap_or(u32::MAX);
        out.entry(k).or_default().push(l);
    }
    out
}

/// Whether two lines differ only in `allowed` fields (the same keys in the
/// same order).
fn differ_only_in(oracle: &str, rust: &str, allowed: &[&str]) -> bool {
    let (w, g) = (fields(oracle), fields(rust));
    w.len() == g.len()
        && w.iter()
            .zip(&g)
            .all(|((ka, va), (kb, vb))| ka == kb && (va == vb || allowed.contains(ka)))
}

/// Whether a D954 case's lines of `table` (one of the seven case tables)
/// differ from the oracle's only where the dock position reaches: the
/// mission line in [`D954_MISSION_FIELDS`]; the offer lines in any number
/// and field but, where both have one, [`D954_OFFER_FIXED`]; the lines of
/// the offer's ships, solars, objectives and comms in number and every
/// field, which the zone, the enemy group and the path make.
fn d954_case_allowed(table: &str, oracle: &[&str], rust: &[&str]) -> bool {
    match table {
        "missions" => {
            oracle.len() == 1
                && rust.len() == 1
                && differ_only_in(oracle[0], rust[0], &D954_MISSION_FIELDS)
        }
        "offers" => oracle
            .iter()
            .zip(rust)
            .all(|(a, b)| D954_OFFER_FIXED.iter().all(|k| field(a, k) == field(b, k))),
        _ => true,
    }
}

/// [`compare`] for one of the seven case tables, with the D954 cases of
/// `d954` allowed to differ as [`d954_case_allowed`] says; each such case
/// that differs is listed in `listed`. Without a D954 case among the lines,
/// exactly [`compare`].
fn compare_cases(
    table: &str,
    oracle: &str,
    rendered: &[String],
    d954: &BTreeSet<u32>,
    bounded: &mut Vec<String>,
    listed: &mut Vec<String>,
) -> Vec<String> {
    let want = group_by(oracle.lines(), "case");
    let got = group_by(rendered.iter().map(String::as_str), "case");
    if !want.keys().chain(got.keys()).any(|c| d954.contains(c)) {
        return compare(table, oracle, rendered, bounded);
    }
    let mut problems = Vec::new();
    let cases: BTreeSet<u32> = want.keys().chain(got.keys()).copied().collect();
    for case in cases {
        let w = want.get(&case).cloned().unwrap_or_default();
        let g = got.get(&case).cloned().unwrap_or_default();
        if d954.contains(&case) {
            if w == g {
                continue;
            }
            if d954_case_allowed(table, &w, &g) {
                listed.push(format!(
                    "{table} case {case} (D954, the base has no dock object): oracle {} lines, rust {}",
                    w.len(),
                    g.len()
                ));
            } else {
                problems.push(format!(
                    "{table} case {case}: a D954 case differs outside the dock position's fields:\n  oracle: {}\n  rust:   {}",
                    w.join(" | ").chars().take(600).collect::<String>(),
                    g.join(" | ").chars().take(600).collect::<String>()
                ));
            }
            continue;
        }
        let w_text = w.join("\n");
        let g_lines: Vec<String> = g.iter().map(|s| (*s).to_owned()).collect();
        problems.extend(
            compare(table, &w_text, &g_lines, bounded)
                .into_iter()
                .map(|p| format!("case {case}: {p}")),
        );
    }
    problems
}

/// Whether two saves differ only in keys outside [`D954_SAVE_FIXED`] (the
/// same lines with the same keys).
fn d954_save_allowed(oracle: &[u8], rust: &[u8]) -> bool {
    let (w, g) = (
        String::from_utf8_lossy(oracle),
        String::from_utf8_lossy(rust),
    );
    let (w, g): (Vec<&str>, Vec<&str>) = (w.lines().collect(), g.lines().collect());
    w.len() == g.len()
        && w.iter().zip(&g).all(|(a, b)| {
            if a == b {
                return true;
            }
            match (a.split_once(" = "), b.split_once(" = ")) {
                (Some((ka, _)), Some((kb, _))) => ka == kb && !D954_SAVE_FIXED.contains(&ka),
                _ => false,
            }
        })
}

/// The saves' numbers (`save=<n>`) of `rms_offers.txt` lines whose base is
/// one of `bases`.
fn d954_saves<'a>(lines: impl Iterator<Item = &'a str>, bases: &BTreeSet<u32>) -> BTreeSet<i64> {
    lines
        .filter(|l| {
            field(l, "base")
                .and_then(|b| b.parse().ok())
                .is_some_and(|b| bases.contains(&b))
        })
        .filter_map(|l| field(l, "save").and_then(|s| s.parse().ok()))
        .filter(|&s: &i64| s >= 0)
        .collect()
}

/// `rms_offers.txt` with the lines of a D954 base allowed to differ in
/// every field but `source` and `base`, and in number (its vendor and bar
/// offers retry on the search's result); each such base that differs is
/// listed. Without a D954 base among the lines, exactly [`compare`].
fn compare_offer_lines(
    oracle: &str,
    rendered: &[String],
    d954: &BTreeSet<u32>,
    bounded: &mut Vec<String>,
    listed: &mut Vec<String>,
) -> Vec<String> {
    let want = group_by(oracle.lines(), "base");
    let got = group_by(rendered.iter().map(String::as_str), "base");
    if !want.keys().chain(got.keys()).any(|b| d954.contains(b)) {
        return compare("offers", oracle, rendered, bounded);
    }
    let mut problems = Vec::new();
    // The bases in the oracle's order.
    let mut order: Vec<u32> = Vec::new();
    for l in oracle.lines().chain(rendered.iter().map(String::as_str)) {
        let b = field(l, "base")
            .and_then(|v| v.parse().ok())
            .unwrap_or(u32::MAX);
        if !order.contains(&b) {
            order.push(b);
        }
    }
    for base in order {
        let w = want.get(&base).cloned().unwrap_or_default();
        let g = got.get(&base).cloned().unwrap_or_default();
        if w == g {
            continue;
        }
        if d954.contains(&base) {
            let kinds_ok = w.iter().zip(&g).all(|(a, b)| {
                field(a, "source") == field(b, "source") && field(a, "base") == field(b, "base")
            });
            if kinds_ok {
                listed.push(format!(
                    "rms_offers base {base} (D954, the base has no dock object): oracle {} lines, rust {}",
                    w.len(),
                    g.len()
                ));
                continue;
            }
        }
        let w_text = w.join("\n");
        let g_lines: Vec<String> = g.iter().map(|s| (*s).to_owned()).collect();
        problems.extend(
            compare("offers", &w_text, &g_lines, bounded)
                .into_iter()
                .map(|p| format!("base {base}: {p}")),
        );
    }
    problems
}

#[test]
fn a_d954_case_may_differ_only_in_the_dock_positions_fields() {
    let bases: BTreeSet<u32> = [1001, 1002, 1003].into();
    let mission = |case: u32, base: u32, zone: u32, reward: i32| {
        format!(
            "case={case} base={base} system=5 group=9 difficulty=3f800000 seed=1 result=0 \
             random_seed=77 player=7 offer_base={base} offer_group=9 enemy_group=12 dest_sys=5 \
             zone={zone} pos=00000000,00000000,00000000 reward={reward} rep_reward=3e19999a \
             rep_hit=be19999a path=TF offers=1 draws=40 serial=3 enemy_uses=1,0"
        )
    };
    let oracle = format!(
        "{}\n{}\n",
        mission(0, 1001, 50, 900),
        mission(1, 2000, 60, 900)
    );
    let d954 = d954_cases(oracle.lines(), &bases);
    assert_eq!(d954, [0].into());
    // The zone of a D954 base's case is listed, not a problem.
    let mut bounded = Vec::new();
    let mut listed = Vec::new();
    let rust = vec![mission(0, 1001, 51, 900), mission(1, 2000, 60, 900)];
    assert!(compare_cases("missions", &oracle, &rust, &d954, &mut bounded, &mut listed).is_empty());
    assert_eq!(listed.len(), 1);
    assert!(listed[0].contains("case 0 (D954"));
    // A field outside the dock position's, or another base, fails.
    let mut listed = Vec::new();
    let rust = vec![mission(0, 1001, 51, 901), mission(1, 2000, 60, 900)];
    assert_eq!(
        compare_cases("missions", &oracle, &rust, &d954, &mut bounded, &mut listed).len(),
        1
    );
    assert!(listed.is_empty());
    let rust = vec![mission(0, 1001, 50, 900), mission(1, 2000, 61, 900)];
    assert_eq!(
        compare_cases("missions", &oracle, &rust, &d954, &mut bounded, &mut listed).len(),
        1
    );
    // Bit-equal D954 cases count as exact.
    let rust = vec![mission(0, 1001, 50, 900), mission(1, 2000, 60, 900)];
    assert!(compare_cases("missions", &oracle, &rust, &d954, &mut bounded, &mut listed).is_empty());
    assert!(listed.is_empty());
    // An offer of a D954 case may come and go, and its ships with it, but
    // the query's values of an offer both have stay exact.
    let offer = |case: u32, system: u32, reward: i32| {
        format!(
            "case={case} machine=2 type=2 system={system} offer_group=9 hostile_group=12 \
             difficulty=3f800000 reward={reward} rep_hit=be19999a rep_reward=3e19999a \
             offer_base=1001 mc_costume=0 voice=4"
        )
    };
    let oracle = format!("{}\n{}\n", offer(0, 5, 900), offer(1, 5, 900));
    let rust = vec![offer(0, 6, 900), offer(1, 5, 900)];
    let mut listed = Vec::new();
    assert!(compare_cases("offers", &oracle, &rust, &d954, &mut bounded, &mut listed).is_empty());
    assert_eq!(listed.len(), 1);
    let rust = vec![offer(0, 6, 901), offer(1, 5, 900)];
    assert_eq!(
        compare_cases("offers", &oracle, &rust, &d954, &mut bounded, &mut listed).len(),
        1
    );
    let rust = vec![offer(1, 5, 900)];
    assert!(compare_cases("offers", &oracle, &rust, &d954, &mut bounded, &mut listed).is_empty());
    let rust = vec![offer(0, 5, 900), offer(1, 6, 900)];
    assert_eq!(
        compare_cases("offers", &oracle, &rust, &d954, &mut bounded, &mut listed).len(),
        1
    );
    let ships = "case=0 component=ships group=0 ship=0 nickname=00";
    let mut listed = Vec::new();
    assert!(compare_cases("offer_ships", ships, &[], &d954, &mut bounded, &mut listed).is_empty());
    assert_eq!(listed.len(), 1);
    // Without a D954 case, the comparison is the exact one.
    let none = BTreeSet::new();
    assert_eq!(
        compare_cases("offer_ships", ships, &[], &none, &mut bounded, &mut listed).len(),
        1
    );
    // A save of a D954 base may differ in the keys the search reaches, not
    // in the others; the offer lines of such a base in every field but
    // their kind.
    let save = |zone: u32, base: u32| {
        format!(
            "[RandomMission]\ntype = DestroyMission\noffer_base = {base}\nobjective_zone = {zone}\n"
        )
    };
    assert!(d954_save_allowed(
        save(50, 1001).as_bytes(),
        save(51, 1001).as_bytes()
    ));
    assert!(!d954_save_allowed(
        save(50, 1001).as_bytes(),
        save(50, 1002).as_bytes()
    ));
    let offers = "save=0 source=bar base=1001 group=9 created=1 zone=50\nsave=1 source=bar base=2000 group=9 created=1 zone=60\n";
    assert_eq!(d954_saves(offers.lines(), &bases), [0].into());
    let mut listed = Vec::new();
    let rust = vec![
        "save=0 source=bar base=1001 group=9 created=0 zone=51".to_owned(),
        "save=1 source=bar base=2000 group=9 created=1 zone=60".to_owned(),
    ];
    assert!(compare_offer_lines(offers, &rust, &bases, &mut bounded, &mut listed).is_empty());
    assert_eq!(listed.len(), 1);
    let rust = vec![
        "save=0 source=bar base=1001 group=9 created=1 zone=50".to_owned(),
        "save=1 source=bar base=2000 group=9 created=1 zone=61".to_owned(),
    ];
    assert_eq!(
        compare_offer_lines(offers, &rust, &bases, &mut bounded, &mut listed).len(),
        1
    );
}

/// The port's own `CreateMission` for every case of the oracle's dump,
/// against the oracle's `rm_<case table>.txt` (the zone search, the enemy
/// pick and the offer all the port's).
#[test]
fn data_dump_created_random_missions() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_DATA_DIR is not set; the created random missions data dump was skipped"
        );
        return;
    };
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_ORACLE_DIR is not set; the created random missions data dump was skipped"
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
    let files = case_files(render_created(&mut cx));
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "created random missions: {} cases, {} with an offer, {} offers",
        files[0].len(),
        files[0].iter().filter(|l| l.contains(" result=0 ")).count(),
        files[1].len()
    );
    let dir = oracle.join("random_missions");
    let read = |table: &str| {
        std::fs::read(dir.join(format!("rm_{table}.txt")))
            .map(|b| String::from_utf8_lossy(&b).into_owned())
    };
    let bases = d954_base_ids(&mut cx);
    let oracle_missions = read("missions").unwrap_or_default();
    let d954 = d954_cases(
        oracle_missions
            .lines()
            .chain(files[0].iter().map(String::as_str)),
        &bases,
    );
    let mut bounded = Vec::new();
    let mut listed = Vec::new();
    let mut problems = Vec::new();
    for (table, lines) in TABLES[10..].iter().zip(&files) {
        let _ = writeln!(summary, "  rm_{table}: {} lines", lines.len());
        match read(table) {
            Ok(text) => problems.extend(compare_cases(
                table,
                &text,
                lines,
                &d954,
                &mut bounded,
                &mut listed,
            )),
            Err(e) => problems.push(format!("rm_{table}.txt: {e}")),
        }
    }
    finish_summary(
        &mut summary,
        &oracle.join("rust-created-random-missions.txt"),
        &problems,
        &bounded,
        &listed,
    );
    assert!(
        problems.is_empty(),
        "created random missions: {} differences from the oracle",
        problems.len()
    );
}

/// What `OracleDumpRandomMissionSaves` makes: the lines of
/// `rms_offers.txt` and the saves, by file name (`rmsave_<n>.ini`, then
/// `rmsave_pair.ini`).
#[derive(Default)]
struct Saves {
    offers: Vec<String>,
    files: Vec<(String, Vec<u8>)>,
}

/// One line of `rms_offers.txt`.
#[allow(clippy::too_many_arguments)]
fn created_line(
    cx: &Cx<'_>,
    save: i32,
    source: &str,
    base: u32,
    group: u32,
    created: bool,
    offer_id: u32,
    kind: &[u8],
    loader: &Loader,
    mission: Option<RmDataKey>,
) -> String {
    let mut line = format!(
        "save={save} source={source} base={base} group={group} created={} offer_id={offer_id} type={}",
        u32::from(created),
        hex(kind)
    );
    let Some(m) = mission_of(cx, mission) else {
        line.push_str(" mission=-");
        return line;
    };
    let _ = write!(
        line,
        " random_seed={} player={} offer_base={} offer_group={} enemy_group={} dest_sys={} zone={} \
         pos={} reward={} rep_reward={} rep_hit={} difficulty={} path={} offers={} serial={} zones={}",
        m.random_seed,
        m.player,
        m.offer_base,
        m.offer_group,
        m.enemy_group,
        m.dest_sys,
        m.objective_zone,
        vector(m.objective_pos),
        m.reward,
        bits(m.rep_reward),
        bits(m.rep_hit),
        bits(m.difficulty),
        m.tree_path.text(),
        m.offers.len(),
        cx.logic.content.state.rm.ship_group_serial,
        cx.logic.content.state.rm.objective_zones.len()
    );
    if m.offers.is_empty() {
        line.push_str(" class=-");
        return line;
    }
    let mut desc = MissionVendorProperties::default();
    let filter = loader.offer_description(cx, mission.expect("a mission"), &mut desc);
    let _ = write!(
        line,
        " class={} title={} description={} briefing={} base_id={} icon={} filter={filter}",
        desc.mission_class,
        fmt_str(&desc.title),
        fmt_str(&desc.description),
        fmt_str(&desc.briefing),
        desc.base_id,
        desc.icon_texture_id
    );
    line
}

fn section(cx: &Cx<'_>, key: Option<RmDataKey>) -> Vec<u8> {
    let mut out = Vec::new();
    if let Some(m) = mission_of(cx, key) {
        mission::write_section(&m, &mut out).expect("in memory");
    }
    out
}

/// The vendor and bar offers of every base of [`BASES`], as the export
/// makes them, with their saves. Player 7 stays registered.
fn make_saves(cx: &mut Cx<'_>) -> Saves {
    let mut out = Saves::default();
    let state = PlayerState::new(cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let save_seed = SEEDS[1];
    for name in BASES {
        let Some((base, system)) = known_base(cx, name) else {
            continue;
        };
        cx.logic.content.state.rm.objective_zones.clear();
        if let Some(s) = cx.logic.content.state.players.get_mut(&PLAYER) {
            s.system = Some(system);
        }
        // BaseSupport loads the base before its vendor offers.
        bases::load_base(cx, base);
        cx.world.rng().seed(save_seed);
        let mut offers = vendor::build_mission_vendor_offers(cx, base, PLAYER);
        out.offers.push(format!(
            "source=vendor_offers base={base} count={} zones={}",
            offers.len(),
            cx.logic.content.state.rm.objective_zones.len()
        ));
        for o in &offers {
            let mission = o.data.mission;
            let save = if mission.is_some() {
                out.files.len() as i32
            } else {
                -1
            };
            let group = mission_of(cx, mission).map_or(0, |m| m.offer_group as u32);
            out.offers.push(created_line(
                cx,
                save,
                "vendor",
                base,
                group,
                true,
                o.offer_id,
                &o.data.kind,
                &o.data.loader,
                mission,
            ));
            if mission.is_some() {
                let name = format!("rmsave_{}.ini", out.files.len());
                out.files.push((name, section(cx, mission)));
            }
        }
        vendor::clear_mission_vendor_offers(cx, &mut offers);
        bases::unload_base(cx, base);
        let factions = mission_factions(cx, base);
        for faction in factions {
            let first = &faction.mission_types[0];
            let npc = BaseNpcMission {
                min_difficulty: first.min_difficulty,
                max_difficulty: first.max_difficulty,
                kind: first.kind.clone(),
            };
            cx.world.rng().seed(save_seed);
            let (created, mut data) =
                vendor::create_bar_mission_offer(cx, &npc, PLAYER, base, faction.group);
            let save = if created && data.mission.is_some() {
                out.files.len() as i32
            } else {
                -1
            };
            out.offers.push(created_line(
                cx,
                save,
                "bar",
                base,
                faction.group,
                created,
                0,
                &data.kind,
                &data.loader,
                data.mission,
            ));
            if save >= 0 {
                let name = format!("rmsave_{}.ini", out.files.len());
                out.files.push((name, section(cx, data.mission)));
            }
            data.release(cx);
        }
    }
    if out.files.len() >= 2 {
        let mut pair = out.files[0].1.clone();
        pair.extend_from_slice(&out.files[1].1);
        out.files.push(("rmsave_pair.ini".to_owned(), pair));
    }
    out
}

/// `LoadOracleRmSaves`: each save loaded by a new loader for player 7, in
/// order, rendered with its number as the case, the loader released.
fn render_loads(cx: &mut Cx<'_>, files: &[(String, Vec<u8>)]) -> [Vec<String>; 7] {
    if !cx.logic.content.state.players.contains_key(&PLAYER) {
        let state = PlayerState::new(cx, PLAYER);
        cx.logic.content.state.players.insert(PLAYER, state);
    }
    let mut out = Rendered::default();
    for (case, (name, bytes)) in files.iter().enumerate() {
        let case = case as u32;
        let mut reader = IniReader::new();
        reader.open(name.as_bytes(), bytes.clone());
        let mut loader = Loader::new();
        let (loaded, key) = loader.load(cx, &mut reader, PLAYER);
        let m = mission_of(cx, Some(key)).expect("the loaded mission");
        let after = cx.world.rng().clone();
        let draw_count = draws(m.random_seed, &after);
        out.missions.push(format!(
            "save={case} loaded={} player={} offer_base={} offer_group={} enemy_group={} dest_sys={} \
             zone={} pos={} reward={} rep_reward={} rep_hit={} difficulty={} jump_pref={} offer_ids={} \
             random_seed={} path={} offers={} draws={draw_count} serial={} zones={} description={} \
             enemy_uses={}",
            u32::from(loaded),
            m.player,
            m.offer_base,
            m.offer_group,
            m.enemy_group,
            m.dest_sys,
            m.objective_zone,
            vector(m.objective_pos),
            m.reward,
            bits(m.rep_reward),
            bits(m.rep_hit),
            bits(m.difficulty),
            m.jump_pref,
            m.offer_ids,
            m.random_seed,
            m.tree_path.text(),
            m.offers.len(),
            cx.logic.content.state.rm.ship_group_serial,
            cx.logic.content.state.rm.objective_zones.len(),
            fmt_str(&m.description),
            name_uses(cx, m.enemy_group)
        ));
        for offer in &m.offers {
            render_offer(cx, &mut out, case, offer);
        }
        loader.release(cx);
    }
    case_files(out)
}

/// The C++ writer's saves against the port's, and the port's loads of the
/// C++'s saves against the C++'s loads of them.
#[test]
fn data_dump_random_mission_saves() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_DATA_DIR is not set; the random mission saves data dump was skipped"
        );
        return;
    };
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_ORACLE_DIR is not set; the random mission saves data dump was skipped"
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
    let dir = oracle.join("random_mission_saves");
    let saves = make_saves(&mut cx);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "random mission saves: {} offer lines, {} saves",
        saves.offers.len(),
        saves.files.len()
    );
    let mut bounded = Vec::new();
    let mut listed = Vec::new();
    let mut problems = Vec::new();
    let bases = d954_base_ids(&mut cx);
    let _ = writeln!(summary, "  rms_offers: {} lines", saves.offers.len());
    let oracle_offers = match std::fs::read(dir.join("rms_offers.txt")) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
        Err(e) => {
            problems.push(format!("rms_offers.txt: {e}"));
            String::new()
        }
    };
    problems.extend(compare_offer_lines(
        &oracle_offers,
        &saves.offers,
        &bases,
        &mut bounded,
        &mut listed,
    ));
    let d954 = d954_saves(
        oracle_offers
            .lines()
            .chain(saves.offers.iter().map(String::as_str)),
        &bases,
    );
    // The C++'s saves, in its order, for the port's loads; a save of a D954
    // base may differ in the keys the search reaches.
    let mut cpp_files = Vec::new();
    for (n, (name, text)) in saves.files.iter().enumerate() {
        match std::fs::read(dir.join(name)) {
            Ok(bytes) => {
                if let Some(p) = first_difference(name, &bytes, text) {
                    if d954.contains(&(n as i64)) && d954_save_allowed(&bytes, text) {
                        listed.push(format!("{name} (D954, the base has no dock object)"));
                    } else {
                        problems.push(p);
                    }
                }
                cpp_files.push((name.clone(), bytes));
            }
            Err(e) => problems.push(format!("{name}: {e}")),
        }
    }
    let _ = writeln!(
        summary,
        "  {} saves compared with the C++ writer",
        saves.files.len()
    );
    let loads = render_loads(&mut cx, &cpp_files);
    problems.extend(compare_files(
        &mut summary,
        &dir,
        "rmload_",
        &LOAD_TABLES,
        &loads,
        &mut bounded,
    ));
    finish_summary(
        &mut summary,
        &oracle.join("rust-random-mission-saves.txt"),
        &problems,
        &bounded,
        &listed,
    );
    assert!(
        problems.is_empty(),
        "random mission saves: {} differences from the oracle",
        problems.len()
    );
}

/// Writes the port's saves into `$OUTPOST_SAVE_LOADS_DIR/rust/` as the
/// server's text-mode save path writes them.
#[test]
fn rust_random_mission_saves_for_the_oracle() {
    let (Some(root), Some(dir)) = (
        std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from),
        std::env::var_os("OUTPOST_SAVE_LOADS_DIR").map(PathBuf::from),
    ) else {
        println!(
            "notice: OUTPOST_DATA_DIR or OUTPOST_SAVE_LOADS_DIR is not set; no random mission saves were written for the oracle"
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
    let saves = make_saves(&mut cx);
    let out = dir.join("rust");
    std::fs::create_dir_all(&out).expect("the saves' folder");
    for (name, text) in &saves.files {
        std::fs::write(out.join(name), text_mode_write(text)).expect("a save");
    }
    println!(
        "{} random mission saves written to {}",
        saves.files.len(),
        out.display()
    );
}

/// The C++ loads of the port's saves against the port's loads of them.
#[test]
fn oracle_loads_of_rust_random_mission_saves() {
    let (Some(root), Some(oracle)) = (
        std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from),
        std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from),
    ) else {
        println!(
            "notice: OUTPOST_DATA_DIR or OUTPOST_ORACLE_DIR is not set; no random mission loads were compared"
        );
        return;
    };
    let rust = oracle.join("rust");
    if !rust.join("rmsave_0.ini").is_file() {
        println!(
            "notice: {} holds no random mission saves; no loads were compared",
            rust.display()
        );
        return;
    }
    let Some((mut world, mut logic)) = started(&root.join("EXE"), &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut files = Vec::new();
    for n in 0.. {
        let name = format!("rmsave_{n}.ini");
        let Ok(bytes) = std::fs::read(rust.join(&name)) else {
            break;
        };
        files.push((name, bytes));
    }
    if let Ok(bytes) = std::fs::read(rust.join("rmsave_pair.ini")) {
        files.push(("rmsave_pair.ini".to_owned(), bytes));
    }
    // P3.7d: the port's save of the random-mission session's mission
    // mid-run (`rm_session.rs`' `rust_rm_save_for_the_oracle`): its
    // `[RandomMission]` section, then the mission object's sections, which
    // the loader passes over.
    if let Ok(bytes) = std::fs::read(rust.join("rmsave_run.ini")) {
        files.push(("rmsave_run.ini".to_owned(), bytes));
    }
    let loads = render_loads(&mut cx, &files);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "random mission save loads: {} restores of the port's saves",
        files.len()
    );
    let mut bounded = Vec::new();
    let problems = compare_files(
        &mut summary,
        &oracle,
        "rmload_",
        &LOAD_TABLES,
        &loads,
        &mut bounded,
    );
    finish_summary(
        &mut summary,
        &oracle.join("rust-random-mission-save-loads.txt"),
        &problems,
        &bounded,
        &[],
    );
    assert!(
        problems.is_empty(),
        "random mission save loads: {} differences from the C++ restores",
        problems.len()
    );
}

/// The first differing line of two texts, or none.
fn first_difference(name: &str, want: &[u8], got: &[u8]) -> Option<String> {
    if want == got {
        return None;
    }
    let want = String::from_utf8_lossy(want);
    let got = String::from_utf8_lossy(got);
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

#[test]
fn every_vendor_faction_has_properties_weights_and_a_dock() {
    // `CollectZonesThroughJumps` and `PickObjectiveZone` read the offering
    // faction's properties without a test (D951): every `[BaseFaction]`
    // with a mission type has them on the shipped data. Every base that
    // offers missions has an object docking with it but three bases whose
    // station a mission creates, where `CreateMission`'s search starts from
    // the unset dock position (D954); and the weights are positive, so no
    // pick falls past the candidates (D952).
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut undocked = Vec::new();
    let mut factions = 0;
    let bases = cx.logic.content.state.bases.clone();
    for (id, base) in &bases {
        if base.factions.iter().all(|f| f.mission_types.is_empty()) {
            continue;
        }
        if mission::find_dock_object(&mut cx, *id) == Vector::ZERO {
            let (_, name) = api::misc::base_nickname(&mut cx, 0x40, *id);
            let name = String::from_utf8_lossy(&name.unwrap_or_default()).to_ascii_lowercase();
            undocked.push(name);
        }
        for f in &base.factions {
            if f.mission_types.is_empty() {
                continue;
            }
            factions += 1;
            assert!(
                cx.logic.content.tables.faction_props(f.group).is_some(),
                "{}",
                f.group
            );
            assert!(f.weight > 0.0, "{}", f.group);
            assert!(
                f.mission_types.iter().all(|t| t.weight > 0.0),
                "{}",
                f.group
            );
        }
    }
    // The three are bases whose station a story mission or
    // `MissionCreatedSolars.ini` creates.
    undocked.sort();
    assert_eq!(undocked, ["fr01_01_base", "he01_15_base", "ve05_02_base"]);
    assert!(factions > 0);
}

#[test]
fn created_missions_save_and_restore_on_the_shipped_data() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let saves = make_saves(&mut cx);
    let vendors = saves
        .offers
        .iter()
        .filter(|l| l.contains(" source=vendor "))
        .count();
    let bars = saves
        .offers
        .iter()
        .filter(|l| l.contains(" source=bar ") && l.contains(" created=1 "))
        .count();
    println!(
        "{} offer lines, {vendors} vendor offers, {bars} bar offers, {} saves",
        saves.offers.len(),
        saves.files.len()
    );
    assert!(vendors > 0 && bars > 0);
    assert!(saves.files.len() > 2);
    // Every mission went with its loader: no mission, objective zone or
    // registered system is left but the bar offers' systems, which
    // `CreateBarMissionOffer` does not release.
    assert!(cx.logic.content.state.rm.missions.entries.is_empty());
    assert!(cx.logic.content.state.rm.objective_zones.is_empty());
    let first = render_loads(&mut cx, &saves.files);
    // Each save rebuilds its mission with an offer; the pair holds two.
    let missions = &first[0];
    assert_eq!(missions.len(), saves.files.len());
    let loaded = missions.iter().filter(|l| l.contains(" loaded=1 ")).count();
    println!("{loaded} of {} loads rebuilt an offer", missions.len());
    assert!(loaded * 10 >= missions.len() * 9);
    assert!(
        missions
            .last()
            .is_some_and(|l| l.contains(" offers=2 ") || l.contains(" offers=1 "))
    );
    assert!(cx.logic.content.state.rm.missions.entries.is_empty());
    // The pair's first zone stays listed: `LoadValue` lists each section's
    // zone, and the mission's destructor removes only its last one.
    assert!(cx.logic.content.state.rm.objective_zones.len() <= 1);
    // The same saves load the same way on a fresh server.
    let (mut world, mut logic) = outdir_started().expect("outdir/");
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let again = make_saves(&mut cx);
    assert_eq!(saves.offers, again.offers);
    assert_eq!(saves.files, again.files);
    assert_eq!(first, render_loads(&mut cx, &again.files));
}

#[test]
fn a_saved_mission_restores_as_a_controller() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let saves = make_saves(&mut cx);
    let (_, text) = saves.files.first().expect("a save");
    let mut save = b"[mPlayer]\nrumor = 5, 2\n\n".to_vec();
    save.extend_from_slice(text);
    cx.logic.content.state.players.remove(&PLAYER);
    let mut state = PlayerState::new(&cx, PLAYER);
    let mut reader = IniReader::new();
    reader.open(b"save.ini", save);
    players::load_player_state(&mut cx, &mut state, &mut reader);
    assert_eq!(state.rumors, [[5, 2]]);
    assert_ne!(state.live_mission, 0);
    let controller = ControllerId(state.live_mission);
    let Some(Instance::RandomMission(r)) = cx.logic.content.instance(controller) else {
        panic!("a DestroyMission controller");
    };
    let key = r.data.expect("its mission");
    // The loader's reference went with the loader; the controller holds two
    // (`FRandomMissionObject::Data` and `FDestroyMission::MissionData`).
    let entry = &cx.logic.content.state.rm.missions.entries[&key];
    assert_eq!(entry.refs, 2);
    assert_eq!(entry.data.offers.len(), 1);
    assert_eq!(entry.data.player, PLAYER);
    assert_eq!(
        cx.logic.content.state.rm.objective_zones,
        [entry.data.objective_zone]
    );
}

#[test]
fn a_vendor_rebuild_releases_what_it_made() {
    let Some((mut world, mut logic)) = outdir_started() else {
        println!("notice: outdir/ is missing; skipped");
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let state = PlayerState::new(&cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let (base, system) = known_base(&mut cx, "he01_01_Base").expect("he01_01_Base");
    if let Some(s) = cx.logic.content.state.players.get_mut(&PLAYER) {
        s.system = Some(system);
    }
    bases::load_base(&mut cx, base);
    let record = cx.logic.content.state.bases[&base].clone();
    assert!(record.vendor_offers_max > 0);
    cx.world.rng().seed(SEEDS[1]);
    let mut offers = vendor::build_mission_vendor_offers(&mut cx, base, PLAYER);
    assert!(!offers.is_empty());
    assert!(offers.len() as i32 <= record.vendor_offers_max);
    // Each offer holds its mission's zone, its id is the player's next, and
    // the loaders' systems were released.
    assert_eq!(
        cx.logic.content.state.rm.objective_zones.len(),
        offers.len()
    );
    for (n, o) in offers.iter().enumerate() {
        assert_eq!(o.offer_id, n as u32 + 1);
        assert_ne!(o.handle, u32::MAX);
    }
    assert!(cx.logic.content.state.rm.dest_systems.is_empty());
    assert!(offers[0].data.loader.can_accept(&mut cx).is_ok());
    vendor::clear_mission_vendor_offers(&mut cx, &mut offers);
    assert!(offers.is_empty());
    assert!(cx.logic.content.state.rm.missions.entries.is_empty());
    assert!(cx.logic.content.state.rm.objective_zones.is_empty());
}
