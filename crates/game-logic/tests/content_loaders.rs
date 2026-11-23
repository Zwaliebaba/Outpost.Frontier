//! Content's other loaders (work package P3.4c, decision record 0008 point
//! 10): the `missions\mBases.ini` records, the specific NPCs, the three
//! system path tables, and each system's encounter parameters, zones and
//! patrol paths with the factions and archetypes of every ship line,
//! rendered as `Content.dll`'s `OracleDumpContentTables` writes them
//! (`CppSource/Content/oracle_content_tables.cpp` and `BaseDB.cpp`'s
//! `OracleContentDumpBases`) for `CppSource/Oracle/content_tables.txt` on a
//! multiplayer start-up.
//!
//! The oracle writes the bases from fresh records that take the live
//! records' factions and are then loaded, the specific NPCs from
//! StoryLauncher's loop (a multiplayer server has no StoryLauncher), and
//! each system from a local record read as `FSystemInfo::LoadFromReader`
//! reads its encounter parameters, zones and patrol paths. The port does
//! the same here with [`build_content_bases`], [`ContentBase::load`],
//! [`load_specific_npcs`], [`SystemPaths::load`],
//! [`load_system_encounter_parameters`], [`enumerate_system_zones`] and
//! [`build_patrol_paths`].
//!
//! `data_dump_content_loaders` reads `OUTPOST_DATA_DIR` and
//! `OUTPOST_ORACLE_DIR` as `content_tables.rs` does; the other tests check
//! values of the shipped data in `outdir/` and return early when it is
//! missing. Nothing these tests read or write is committed.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::content::bases::{ContentBase, ContentBases, build_content_bases};
use game_logic::content::cargo::{faction_ship_line_archs, ship_line_factions};
use game_logic::content::encounter_params::{
    EncounterParameters, SystemEncounterParameters, load_system_encounter_parameters,
};
use game_logic::content::npc_desc::{
    NpcCharacterDesc, SPECIFIC_NPC_FILE, SpecificNpcs, load_specific_npcs,
};
use game_logic::content::patrol_paths::{PatrolPath, build_patrol_paths};
use game_logic::content::system_paths::{PathTable, SystemPaths};
use game_logic::content::tables::ContentTables;
use game_logic::content::zones::{SystemZone, enumerate_system_zones};
use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::costume::Costume;
use game_sim::data::{DataSource, open_ini};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

/// The tables in the oracle's order, each `content_<name>.txt`.
const TABLES: [&str; 12] = [
    "bases",
    "base_npcs",
    "base_rooms",
    "specific_npcs",
    "system_paths_all",
    "system_paths_gate",
    "system_paths_hole",
    "encounter_params",
    "encounter_formations",
    "ship_line_factions",
    "zones",
    "patrol_paths",
];

/// One system as the oracle reads it.
struct SystemLoad {
    system: u32,
    parameters: SystemEncounterParameters,
    zones: Vec<SystemZone>,
    paths: Vec<PatrolPath>,
}

/// Everything the loaders read.
struct Loaded {
    tables: ContentTables,
    bases: ContentBases,
    loaded_bases: Vec<ContentBase>,
    specific_npcs: SpecificNpcs,
    paths: SystemPaths,
    systems: Vec<SystemLoad>,
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

fn join<T: ToString>(values: &[T], separator: &str) -> String {
    values
        .iter()
        .map(ToString::to_string)
        .collect::<Vec<_>>()
        .join(separator)
}

fn costume(c: &Costume) -> String {
    let mut text = format!(
        "{},{},{},{},{}",
        c.part_ids[0],
        c.part_ids[1],
        c.part_ids[2],
        c.part_ids[3],
        c.accessory_ids.len()
    );
    for a in &c.accessory_ids {
        let _ = write!(text, ",{a}");
    }
    text
}

fn desc(d: &NpcCharacterDesc) -> String {
    format!(
        " costume={} voice={} name={} affiliation={} nickname={}",
        costume(&d.appearance),
        d.voice,
        d.individual_name,
        d.affiliation,
        d.nickname
    )
}

fn weights(list: &[(u32, f32)]) -> String {
    list.iter()
        .map(|(g, w)| format!("{g}:{}", bits(*w)))
        .collect::<Vec<_>>()
        .join("+")
}

fn render_paths(out: &mut Vec<String>, table: &PathTable) {
    for (from, paths) in table {
        for (to, path) in paths {
            out.push(format!("from={from} to={to} path={}", join(path, ",")));
        }
    }
}

fn render_parameters(
    out: &mut Vec<String>,
    formations: &mut Vec<String>,
    line_factions: &mut Vec<String>,
    tables: &ContentTables,
    system: u32,
    id: u32,
    p: &EncounterParameters,
) {
    let permutations: Vec<String> = p
        .permutations
        .iter()
        .map(|perm| {
            let entries: Vec<String> = perm
                .entries
                .iter()
                .map(|e| format!("{}{}", e.formation_index, if e.star { "*" } else { "" }))
                .collect();
            format!("{}:{}", bits(perm.weight), entries.join("+"))
        })
        .collect();
    out.push(format!(
        "system={system} params={id} nickname={} system_id={} zone={} reroll={} last={} difficulty={} formations={} permutations={}",
        p.nickname,
        p.system,
        p.zone,
        bits(p.reroll_interval),
        bits(p.last_reroll_time),
        p.difficulty,
        p.formations.len(),
        permutations.join(";")
    ));
    for (f, e) in p.formations.iter().enumerate() {
        let ships: Vec<String> = e
            .ships
            .iter()
            .map(|s| {
                format!(
                    "{}/{}/{}/{}/{}/{}/{}",
                    join(&s.ship_classes, "+"),
                    s.npc_arch,
                    s.pilot_job,
                    s.min_count,
                    s.max_count,
                    join(&s.make_classes, "+"),
                    s.difficulty_offset
                )
            })
            .collect();
        formations.push(format!(
            "system={system} params={id} formation={f} feeling={} reroll={} mask={} f0c={} value={} faction={} formation_id={} by_class={} behavior={} arrival={} distance={:016x} times={} simultaneous={} f78={} longevity={} ships={}",
            e.feeling_to_formation,
            e.reroll_frequency,
            e.feeling_mask,
            e.field_0x0c,
            e.feeling_value,
            e.faction,
            e.formation,
            e.formation_by_class,
            e.behavior,
            join(&e.arrival, ""),
            e.zone_creation_distance.to_bits(),
            e.times_to_create,
            u32::from(e.allow_simultaneous_creation),
            e.field_0x78,
            e.longevity,
            ships.join(";")
        ));
        for (s, ship) in e.ships.iter().enumerate() {
            let factions = ship_line_factions(tables, ship);
            let archs: Vec<String> = factions
                .iter()
                .map(|&g| {
                    format!(
                        "{g}:{}",
                        join(&faction_ship_line_archs(tables, g, ship), "+")
                    )
                })
                .collect();
            line_factions.push(format!(
                "system={system} params={id} formation={f} ship={s} factions={} archs={}",
                join(&factions, ","),
                archs.join(";")
            ));
        }
    }
}

fn render_zone(system: u32, z: &SystemZone) -> String {
    let p = &z.population;
    let restrictions: Vec<String> = p
        .density_restrictions
        .iter()
        .map(|r| format!("{}:{}", r.max_count, r.kind))
        .collect();
    let encounters: Vec<String> = p
        .encounters
        .iter()
        .map(|e| {
            let classes: Vec<u32> = e
                .parameters
                .formations
                .iter()
                .flat_map(|f| f.ships.iter().flat_map(|s| s.ship_classes.iter().copied()))
                .collect();
            format!(
                "{}/{}/{}/{}/{}/{}",
                e.parameters.nickname,
                e.parameters.zone,
                e.parameters.difficulty,
                bits(e.chance),
                weights(&e.parameters.factions),
                join(&classes, "+")
            )
        })
        .collect();
    format!(
        "system={system} zone={} nickname={} path={} unpopulated={} tradelane={} lawful={} unlawful={} vignette={} density={} repop={} keys={} attack={} restrictions={} f1c={} f30={} additive={} battle={} relief={} encounters={}",
        z.zone_id,
        z.nickname,
        z.path.map_or(-1, |p| p as i64),
        u32::from(z.unpopulated),
        u32::from(z.tradelane),
        u32::from(z.lawful),
        u32::from(z.unlawful),
        z.vignette_type,
        p.density,
        bits(p.repop_time),
        u32::from(p.has_population_keys),
        bits(p.tradelane_attack),
        restrictions.join(","),
        bits(p.field_0x1c),
        bits(p.field_0x30),
        u32::from(p.population_additive),
        p.max_battle_size,
        bits(p.relief_time),
        encounters.join(";")
    )
}

fn render_path(system: u32, index: usize, p: &PatrolPath) -> String {
    let points: Vec<String> = p
        .points
        .iter()
        .map(|v| format!("{}:{}:{}", bits(v.x), bits(v.y), bits(v.z)))
        .collect();
    let zones: Vec<String> = p
        .zone_steps
        .iter()
        .map(|(z, steps)| format!("{z}:{}", join(steps, "+")))
        .collect();
    let objects: Vec<String> = p
        .objects
        .iter()
        .map(|o| format!("{}:{}", o.object_id, join(&o.point_indices, "+")))
        .collect();
    format!(
        "system={system} path={index} label={} usage={} eligible={} loop={} points={} zones={} objects={}",
        p.label,
        p.usage,
        u32::from(p.mission_eligible),
        u32::from(p.looped),
        points.join(","),
        zones.join(","),
        objects.join(",")
    )
}

/// Every table's lines, in [`TABLES`] order.
fn render(l: &Loaded) -> Vec<Vec<String>> {
    let mut out: Vec<Vec<String>> = vec![Vec::new(); TABLES.len()];
    for base in &l.loaded_bases {
        let live = &l.bases[&base.base_id];
        let factions: Vec<String> = live
            .factions
            .iter()
            .map(|f| {
                let kinds: Vec<String> = f
                    .mission_types
                    .iter()
                    .map(|t| {
                        format!(
                            "{}/{}/{}",
                            bits(t.min_difficulty),
                            bits(t.max_difficulty),
                            bits(t.weight)
                        )
                    })
                    .collect();
                format!(
                    "{}:{}:{}:{}",
                    f.group,
                    bits(f.weight),
                    join(&f.npcs, "+"),
                    kinds.join("+")
                )
            })
            .collect();
        out[0].push(format!(
            "base={} msg_id_prefix={} diff={} local_faction={} vendor={},{} npcs={} rooms={} factions={}",
            base.base_id,
            hex(&live.msg_id_prefix),
            live.diff,
            live.local_faction,
            base.vendor_offers_min,
            base.vendor_offers_max,
            base.npcs.len(),
            base.rooms.len(),
            factions.join(";")
        ));
        for (key, npc) in &base.npcs {
            let missions: Vec<String> = npc
                .missions
                .iter()
                .map(|m| {
                    format!(
                        "{}/{}/{}",
                        bits(m.min_difficulty),
                        bits(m.max_difficulty),
                        hex(&m.kind)
                    )
                })
                .collect();
            let bribes: Vec<String> = npc
                .bribes
                .iter()
                .map(|b| format!("{}/{}/{}", b.cost, b.group, b.text_ids))
                .collect();
            let knows: Vec<String> = npc
                .knows
                .iter()
                .map(|k| {
                    format!(
                        "{}/{}/{}/{}",
                        k.price, k.field_0x04, k.field_0x08, k.rep_level
                    )
                })
                .collect();
            let rumors: Vec<String> = npc
                .rumors
                .iter()
                .map(|r| {
                    format!(
                        "{}/{}/{}/{}",
                        r.from_stage,
                        r.to_stage,
                        r.rep_level,
                        join(&r.ids, "+")
                    )
                })
                .collect();
            out[1].push(format!(
                "base={} npc={key}{} missions={} bribes={} knows={} rumors={} rooms={}",
                base.base_id,
                desc(&npc.desc),
                missions.join(","),
                bribes.join(","),
                knows.join(","),
                rumors.join(","),
                join(&npc.rooms, ",")
            ));
        }
        for room in &base.rooms {
            let fixtures: Vec<String> = room
                .fixtures
                .iter()
                .map(|f| {
                    format!(
                        "{}/{}/{}/{}",
                        f.npc,
                        f.placement,
                        hex(&f.fidget),
                        hex(&f.role)
                    )
                })
                .collect();
            out[2].push(format!(
                "base={} room={} density={} fixtures={}",
                base.base_id,
                room.room_id,
                room.character_density,
                fixtures.join(",")
            ));
        }
    }
    for (key, d) in &l.specific_npcs {
        out[3].push(format!("npc={key}{}", desc(d)));
    }
    render_paths(&mut out[4], &l.paths.all);
    render_paths(&mut out[5], &l.paths.gate);
    render_paths(&mut out[6], &l.paths.hole);
    let (mut params, mut formations, mut line_factions) = (Vec::new(), Vec::new(), Vec::new());
    for s in &l.systems {
        for (id, p) in &s.parameters {
            render_parameters(
                &mut params,
                &mut formations,
                &mut line_factions,
                &l.tables,
                s.system,
                *id,
                p,
            );
        }
        for z in &s.zones {
            out[10].push(render_zone(s.system, z));
        }
        for (index, p) in s.paths.iter().enumerate() {
            out[11].push(render_path(s.system, index, p));
        }
    }
    out[7] = params;
    out[8] = formations;
    out[9] = line_factions;
    out
}

/// The loaders over a data root: the engine INI under `exe`, the data
/// under `data`; a multiplayer world, as the oracle's server is.
fn load(exe: &Path, data_dir: &Path) -> Option<(Loaded, usize)> {
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let host = Host::new(data_dir);
    let tables = ContentTables::load(&mut cx, &host);

    let mut bases = ContentBases::new();
    build_content_bases(&mut cx, &mut bases, &host);
    let mut loaded_bases = Vec::new();
    for live in bases.values() {
        let mut base = ContentBase::new(live.base_id);
        base.diff = live.diff;
        base.local_faction = live.local_faction;
        base.factions = live.factions.clone();
        base.load(&mut cx, None, &host);
        loaded_bases.push(base);
    }

    let mut reader = open_ini(&host, SPECIFIC_NPC_FILE).unwrap_or_default();
    let specific_npcs = load_specific_npcs(&mut cx, &mut reader);

    let mut paths = SystemPaths::default();
    paths.init(&mut cx, &host);

    let mut ids = Vec::new();
    let mut next = 0;
    while api::misc::next_system_id(&cx, &mut next) {
        ids.push(next);
    }
    ids.sort_unstable();
    let mut systems = Vec::new();
    for system in ids {
        let Ok(mut reader) = api::misc::build_system_reader(&mut cx, &host, system) else {
            continue;
        };
        let parameters = load_system_encounter_parameters(&mut reader, system, &mut |file| {
            open_ini(&host, file)
        });
        let mut zones = enumerate_system_zones(&mut cx, &host, system, true, &parameters);
        let paths = build_patrol_paths(&mut cx, system, &mut zones.zones, &mut zones.path_labels);
        systems.push(SystemLoad {
            system,
            parameters,
            zones: zones.zones,
            paths,
        });
    }
    let reports = cx.logic.reports.0.len();
    for r in cx.logic.reports.0.iter().take(10) {
        println!("report: {r:?}");
    }
    Some((
        Loaded {
            tables,
            bases,
            loaded_bases,
            specific_npcs,
            paths,
            systems,
        },
        reports,
    ))
}

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn outdir_loaded() -> Option<Loaded> {
    let outdir = outdir();
    load(&outdir, &outdir.join("data")).map(|(l, _)| l)
}

/// The bound of the Tier B fields (decision T11): a zone's orientation
/// goes through `FSIN` and `FCOS` in the reference and through Rust's
/// `sin` and `cos` in the port, so game-sim's data dump compares it within
/// this bound (`crates/game-sim/tests/data_dump.rs`). A patrol path's
/// points are its zones' ends, moved along that orientation, and are
/// compared within the same bound.
const POINT_ULPS: u32 = 4;
const POINT_ABSOLUTE: f32 = 1.0e-7;

/// Two float bit patterns within [`POINT_ULPS`] or [`POINT_ABSOLUTE`].
fn within_bound(a: u32, b: u32) -> bool {
    let (x, y) = (f32::from_bits(a), f32::from_bits(b));
    if !x.is_finite() || !y.is_finite() {
        return a == b;
    }
    if (f64::from(x) - f64::from(y)).abs() <= f64::from(POINT_ABSOLUTE) {
        return true;
    }
    // Ordered integers of the two floats; equal signs only.
    let order = |v: u32| {
        if v & 0x8000_0000 != 0 {
            -i64::from(v & 0x7fff_ffff)
        } else {
            i64::from(v)
        }
    };
    (order(a) - order(b)).unsigned_abs() <= u64::from(POINT_ULPS)
}

/// Whether two `patrol_paths` lines are equal but for their points, and
/// those within the Tier B bound.
fn paths_within_bound(oracle: &str, rust: &str) -> bool {
    let (w, g): (Vec<&str>, Vec<&str>) = (oracle.split(' ').collect(), rust.split(' ').collect());
    if w.len() != g.len() {
        return false;
    }
    let floats = |field: &str| -> Option<Vec<u32>> {
        field
            .strip_prefix("points=")?
            .split([',', ':'])
            .filter(|t| !t.is_empty())
            .map(|t| u32::from_str_radix(t, 16).ok())
            .collect()
    };
    w.iter().zip(&g).all(|(a, b)| {
        if a == b {
            return true;
        }
        match (floats(a), floats(b)) {
            (Some(x), Some(y)) => {
                x.len() == y.len() && x.iter().zip(&y).all(|(p, q)| within_bound(*p, *q))
            }
            _ => false,
        }
    })
}

/// The differences of one table from the oracle's file; a line that
/// differs only within the Tier B bound is listed in `bounded`.
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
    let cut = |s: &str| s.chars().take(300).collect::<String>();
    for (i, (w, g)) in want.iter().zip(rendered).enumerate() {
        if *w != g {
            if table == "patrol_paths" && paths_within_bound(w, g) {
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
            break;
        }
    }
    problems
}

#[test]
fn data_dump_content_loaders() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the content loaders data dump was skipped");
        return;
    };
    let Some((loaded, reports)) = load(&root.join("EXE"), &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let rendered = render(&loaded);
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "content loaders: {} tables, {} lines, {} load reports",
        rendered.len(),
        rendered.iter().map(Vec::len).sum::<usize>(),
        reports
    );
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let dir = oracle.join("content");
    let mut problems = Vec::new();
    let mut bounded = Vec::new();
    for (table, lines) in TABLES.iter().zip(&rendered) {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
        let path = dir.join(format!("content_{table}.txt"));
        match std::fs::read(&path) {
            Ok(bytes) => problems.extend(compare(
                table,
                &String::from_utf8_lossy(&bytes),
                lines,
                &mut bounded,
            )),
            Err(e) => problems.push(format!("{}: {e}", path.display())),
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
        "{} lines equal within the Tier B bound ({POINT_ULPS} ulps or {POINT_ABSOLUTE:e})",
        bounded.len()
    );
    for b in bounded.iter().take(5) {
        let _ = writeln!(summary, "{b}");
    }
    for p in problems.iter().take(25) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-content-loaders.txt"), &summary);
    assert!(
        problems.is_empty(),
        "content loaders: {} differences from the oracle",
        problems.len()
    );
}

/// The sizes, counted from the INI files independently.
#[test]
fn content_loaders_counts_on_outdir() {
    let Some(l) = outdir_loaded() else {
        println!("outdir/ is missing: no content loaders");
        return;
    };
    let rendered = render(&l);
    for (table, lines) in TABLES.iter().zip(&rendered) {
        println!("{table}: {} lines", lines.len());
    }
    // One record per universe base; 194 `[MBase]` sections.
    assert_eq!(l.bases.len(), 197);
    // 177 `msg_id_prefix` lines; 161 of the 194 `[MVendor]`s give offers.
    let with_prefix = l
        .bases
        .values()
        .filter(|b| !b.msg_id_prefix.is_empty())
        .count();
    assert_eq!(with_prefix, 177);
    let with_vendor = l
        .loaded_bases
        .iter()
        .filter(|b| b.vendor_offers_max != 0)
        .count();
    assert_eq!(with_vendor, 161);
    assert_eq!(
        l.bases.values().map(|b| b.factions.len()).sum::<usize>(),
        609
    );
    assert_eq!(
        l.loaded_bases.iter().map(|b| b.npcs.len()).sum::<usize>(),
        1642
    );
    assert_eq!(
        l.loaded_bases.iter().map(|b| b.rooms.len()).sum::<usize>(),
        412
    );
    assert_eq!(
        l.loaded_bases
            .iter()
            .flat_map(|b| b.npcs.values())
            .map(|n| n.rumors.len())
            .sum::<usize>(),
        15_127
    );
    assert_eq!(l.specific_npcs.len(), 107);
    // The first `[SystemConnections]` of each source system, the first
    // `Path` to each destination.
    let count = |t: &PathTable| t.values().map(|p| p.len()).sum::<usize>();
    assert_eq!((l.paths.all.len(), count(&l.paths.all)), (50, 2079));
    assert_eq!((l.paths.gate.len(), count(&l.paths.gate)), (35, 1225));
    assert_eq!((l.paths.hole.len(), count(&l.paths.hole)), (46, 2071));
    assert_eq!(
        l.systems.iter().map(|s| s.parameters.len()).sum::<usize>(),
        1146
    );
    // 5,428 `[Zone]`s, 744 of them unpopulated without a path label or a
    // mission type, and Zone_he04_path_navy5_2, which the universe does not
    // enumerate (its id is already in the universe's zone map); 3,038 path
    // labels over 866 labels.
    assert_eq!(l.systems.iter().map(|s| s.zones.len()).sum::<usize>(), 4683);
    assert_eq!(l.systems.iter().map(|s| s.paths.len()).sum::<usize>(), 866);
    let encounters: usize = l
        .systems
        .iter()
        .flat_map(|s| s.zones.iter())
        .map(|z| z.population.encounters.len())
        .sum();
    // D797: every faction has properties, every `npc_ship` is defined.
    for &group in &l.tables.factions.groups {
        let props = l.tables.faction_props(group).expect("faction properties");
        assert!(
            props
                .npc_ships
                .iter()
                .all(|&a| l.tables.npc_ship(a).is_some())
        );
    }
    // 5,039 `encounter` lines, one of them in Zone_he04_path_navy5_2.
    assert_eq!(encounters, 5038);
}

/// One entry of each table, its fields read off the INI files.
#[test]
fn content_loaders_known_values_on_outdir() {
    let Some(l) = outdir_loaded() else {
        println!("outdir/ is missing: no content loaders");
        return;
    };
    let id = |name: &str| create_id(name.as_bytes());
    let f = |text: &str| text.parse::<f64>().map_or(f32::NAN, |v| v as f32);
    let outdir = outdir();
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini").expect("config");
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };

    // MISSIONS/mbases.ini, the first [MBase].
    let base_id = id("he01_01_base");
    let live = &l.bases[&base_id];
    assert_eq!(live.msg_id_prefix, b"gcs_refer_base_he01_01_Base");
    assert_eq!(live.diff, 1);
    assert_eq!(live.local_faction, group(&cx, "he_p_grp"));
    let faction = &live.factions[0];
    assert_eq!(faction.group, group(&cx, "he_n_grp"));
    assert_eq!(faction.weight, 13.0);
    assert_eq!(
        faction.npcs,
        [
            id("he0101_lnavy_001_f"),
            id("he0101_lnavy_002_m"),
            id("he0101_lnavy_003_f")
        ]
    );
    let kind = &faction.mission_types[0];
    assert_eq!(kind.min_difficulty, 0.0);
    assert_eq!(kind.max_difficulty, f("0.112387002"));
    assert_eq!(kind.weight, 30.0);
    assert_eq!(kind.kind, b"DestroyMission");
    let base = l
        .loaded_bases
        .iter()
        .find(|b| b.base_id == base_id)
        .expect("base");
    assert_eq!((base.vendor_offers_min, base.vendor_offers_max), (8, 12));

    // Its first [GF_NPC], he0101_ageira_001_f, listed by co_vr_grp's
    // [BaseFaction] (its own affiliation line says the same).
    let npc = &base.npcs[&id("he0101_ageira_001_f")];
    assert_eq!(npc.desc.nickname, id("he0101_ageira_001_f"));
    assert_eq!(npc.desc.individual_name, 220_001);
    assert_eq!(npc.desc.voice, id("rvp501"));
    assert_eq!(npc.desc.affiliation, group(&cx, "co_vr_grp"));
    assert_ne!(npc.desc.appearance.part_ids[1], 0);
    assert_eq!(npc.rooms, [api::gf::find_location(base_id, b"bar")]);
    assert_eq!(npc.bribes.len(), 5);
    assert_eq!(npc.bribes[0].group, group(&cx, "he_p_grp"));
    assert_eq!(
        (npc.bribes[0].cost, npc.bribes[0].text_ids),
        (10_000, 16_100)
    );
    assert_eq!(npc.bribes[4].text_ids, 16_101);
    assert_eq!(npc.rumors.len(), 37);
    let rumor = npc.rumors[0];
    assert_eq!(
        (rumor.from_stage, rumor.to_stage, rumor.rep_level),
        (0, 0x2a, 1)
    );
    assert_eq!(rumor.ids, [131_196, 0, 0, 0, 0, 0, 0, 0]);
    let rumor = npc.rumors[3];
    assert_eq!((rumor.from_stage, rumor.to_stage), (0x01, 0x02));
    let know = npc.knows[0];
    assert_eq!(
        (know.field_0x08, know.field_0x04, know.price, know.rep_level),
        (251_760, 253_608, 300, 1)
    );
    assert!(npc.missions.is_empty());

    // Its first [MRoom]. No room file is loaded at start-up, so no
    // placement is found and no fixture is kept.
    let room = &base.rooms[0];
    assert_eq!(room.room_id, api::gf::find_location(base_id, b"bar"));
    assert_eq!(room.character_density, 7);
    assert!(base.rooms.iter().all(|r| r.fixtures.is_empty()));
    // With the base's rooms loaded, as when a player docks, it is.
    let host = Host::new(outdir.join("data"));
    game_logic::bases::load_base(&mut cx, &host, base_id);
    let mut docked = ContentBase::new(base_id);
    docked.factions = live.factions.clone();
    docked.load(&mut cx, None, &host);
    let fixture = &docked.rooms[0].fixtures[0];
    assert_eq!(fixture.npc, id("he0101_fix_bartender"));
    assert_eq!(fixture.fidget, b"scripts\\vendors\\li_bartender_fidget.thn");
    assert_eq!(fixture.role, b"bartender");
    assert!(fixture.placement >= 0);

    // MISSIONS/specific_npc.ini, test_npc_01: base_appr only.
    let s = &l.specific_npcs[&id("test_npc_01")];
    assert_eq!((s.individual_name, s.voice), (1, id("pilot_f_mil_m01")));
    assert_eq!(s.affiliation, u32::MAX);
    assert_ne!(s.appearance.part_ids[1], 0);

    // UNIVERSE/systems_shortest_path.ini, pe01 to he01.
    assert_eq!(
        l.paths.path(id("pe01"), id("he01"), 0),
        [
            "pe01", "oe01", "fr08", "or03", "or02", "fc06", "he03", "he01"
        ]
        .map(id)
    );
    assert_eq!(l.paths.path(id("pe01"), id("pe01"), 0), [id("pe01")]);
    assert_eq!(l.paths.path_length(id("pe01"), id("he01"), 0), 8);
    assert_eq!(l.paths.path_length(id("pe01"), id("he01"), 3), -1);

    // he01 (UNIVERSE/SYSTEMS/LI01/li01.ini): tradelane_armored_prisoner
    // (MISSIONS/ENCOUNTERS/tradelane_armored_prisoner.ini).
    let he01 = l
        .systems
        .iter()
        .find(|s| s.system == id("he01"))
        .expect("he01");
    let p = &he01.parameters[&id("tradelane_armored_prisoner")];
    assert_eq!(
        (p.nickname, p.system, p.zone),
        (id("tradelane_armored_prisoner"), id("he01"), 0)
    );
    assert_eq!(
        (p.difficulty, p.last_reroll_time, p.reroll_interval),
        (-1, -1.0, 0.0)
    );
    assert_eq!(p.formations.len(), 1);
    assert_eq!(p.permutations.len(), 1);
    assert_eq!(p.permutations[0].weight, 3.0);
    assert_eq!(p.permutations[0].entries[0].formation_index, 0);
    assert!(!p.permutations[0].entries[0].star);
    let e = &p.formations[0];
    assert_eq!(e.ships.len(), 3);
    assert_eq!(e.ships[0].ship_classes, [id("sc_armoreds")]);
    assert_eq!(e.ships[0].pilot_job, id("prisoner_job"));
    assert_eq!(
        (e.ships[2].difficulty_offset, e.ships[2].pilot_job),
        (-1, id("escort_job"))
    );
    assert_eq!(e.formation_by_class, id("gunboats"));
    assert_eq!(e.formation, 0);
    assert_eq!(e.behavior, 2);
    assert_eq!(e.arrival, [0, 1, 0, 0, 0, 0, 0, 0, 0]);
    assert!(e.allow_simultaneous_creation);
    assert_eq!((e.zone_creation_distance, e.times_to_create), (0.0, -1));
    assert_eq!(e.longevity, 24);
    assert!(!ship_line_factions(&l.tables, &e.ships[1]).is_empty());

    // Its first zone, Zone_he01_001_Planet_he01_01.
    let zone = he01
        .zones
        .iter()
        .find(|z| z.nickname == id("Zone_he01_001_Planet_he01_01"))
        .expect("zone");
    let pop = &zone.population;
    assert_eq!(
        (pop.density, pop.repop_time, pop.max_battle_size),
        (12, 10.0, 8)
    );
    assert_eq!(pop.relief_time, 10.0);
    assert_eq!(pop.density_restrictions[0].max_count, 4);
    assert_eq!(pop.density_restrictions[0].kind, id("unlawfuls"));
    assert!(pop.has_population_keys && !zone.unpopulated);
    let first = &pop.encounters[0];
    assert_eq!(first.parameters.nickname, id("area_defend"));
    assert_eq!(
        (first.parameters.zone, first.parameters.difficulty),
        (zone.zone_id, 1)
    );
    assert_eq!(first.chance, f("0.0700000003"));
    assert_eq!(first.parameters.factions, [(group(&cx, "he_p_grp"), 1.0)]);
    let freighter = &pop.encounters[3];
    assert_eq!(freighter.parameters.nickname, id("area_trade_freighter"));
    assert_eq!(freighter.parameters.factions.len(), 4);
    assert_eq!(
        freighter.parameters.factions[3],
        (group(&cx, "co_vr_grp"), f("0.209999993"))
    );
    // `ApplyDifficulty(1)` tags every ship line that has classes.
    assert!(
        first.parameters.formations[0].ships[0]
            .ship_classes
            .contains(&id("diff1"))
            || first.parameters.formations[0].ships[0]
                .ship_classes
                .contains(&id("diff2"))
    );

    // The patrol path outcasts1 and its first zone.
    let path_zone = he01
        .zones
        .iter()
        .find(|z| z.nickname == id("Zone_he01_path_outcasts1_1"))
        .expect("path zone");
    let path = &he01.paths[path_zone.path.expect("on a path")];
    assert_eq!(path.label, id("outcasts1"));
    assert_eq!(path.zone_steps[&path_zone.zone_id], [0]);
    assert_eq!(
        path.points.len(),
        path.zone_steps.values().map(Vec::len).sum::<usize>() + 1
    );
}

/// Without the files every loader reads nothing.
#[test]
fn content_loaders_without_files() {
    let empty = std::env::temp_dir().join("outpost-content-loaders-none");
    let host = Host::new(&empty);
    let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut bases = ContentBases::new();
    build_content_bases(&mut cx, &mut bases, &host);
    assert!(bases.is_empty());
    let mut paths = SystemPaths::default();
    assert!(!paths.init(&mut cx, &host));
    assert!(host.read(b"missions\\specific_npc.ini").is_none());
    assert!(load_specific_npcs(&mut cx, &mut IniReader::default()).is_empty());
}

/// The `[SystemConnections]` writer: the tables written and read back are
/// the tables, with the C++'s blank line after each header.
#[test]
fn system_paths_round_trip_on_outdir() {
    let Some(l) = outdir_loaded() else {
        println!("outdir/ is missing: no content loaders");
        return;
    };
    let outdir = outdir();
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini").expect("config");
    let mut data = GameData::new();
    let host = Host::new(outdir.join("data"));
    data.load(&host, &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let names = game_logic::content::system_paths::system_names(&mut cx, &host);
    assert_eq!(names[&id_of("pe01")], b"pe01");
    let (mut all, mut gate, mut hole) = (Vec::new(), Vec::new(), Vec::new());
    l.paths
        .write(&names, Some(&mut all), Some(&mut gate), Some(&mut hole))
        .expect("written");
    assert!(all.starts_with(b"[SystemConnections]\n\nPath = "));
    let mut back = PathTable::new();
    for bytes in [&all] {
        let mut reader = IniReader::default();
        reader.open(b"written.ini", bytes.clone());
        while reader.read_header() {
            game_logic::content::system_paths::load_section(&mut cx, &mut reader, &mut back);
        }
    }
    assert_eq!(back, l.paths.all);
    // Without the first file the gate file has no path systems.
    let mut gate_only = Vec::new();
    l.paths
        .write(&names, None, Some(&mut gate_only), None)
        .expect("written");
    let text = String::from_utf8_lossy(&gate_only);
    let line = text
        .lines()
        .find(|l| l.starts_with("Path = "))
        .expect("a path");
    assert_eq!(line.matches(", ").count(), 1);
    assert_ne!(gate, gate_only);
}

/// `BuildSystemPathTables` over the shipped universe, with no gate locked.
/// The shipped files were not written by it (they have no blank line after
/// each header, and fewer source systems): about a quarter of their paths
/// are longer than the shortest ones it finds, none is shorter, and every
/// path it finds runs from its source to its destination.
#[test]
fn system_paths_build_on_outdir() {
    let Some(l) = outdir_loaded() else {
        println!("outdir/ is missing: no content loaders");
        return;
    };
    let outdir = outdir();
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini").expect("config");
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut systems = Vec::new();
    let mut next = 0;
    while api::misc::next_system_id(&cx, &mut next) {
        systems.push(next);
    }
    systems.sort_unstable();
    let mut built = SystemPaths::default();
    assert!(built.build(&mut cx, &systems, &|_| false));
    assert!(!built.build(&mut cx, &systems, &|_| false));
    for (name, a, b) in [
        ("all", &built.all, &l.paths.all),
        ("gate", &built.gate, &l.paths.gate),
        ("hole", &built.hole, &l.paths.hole),
    ] {
        let count = |t: &PathTable| t.values().map(|p| p.len()).sum::<usize>();
        let same = a
            .iter()
            .flat_map(|(f, p)| p.iter().map(move |(t, path)| (f, t, path)))
            .filter(|(f, t, path)| b.get(f).and_then(|p| p.get(t)) == Some(path))
            .count();
        let same_length = a
            .iter()
            .flat_map(|(f, p)| p.iter().map(move |(t, path)| (f, t, path)))
            .filter(|(f, t, path)| {
                b.get(f)
                    .and_then(|p| p.get(t))
                    .is_some_and(|q| q.len() == path.len())
            })
            .count();
        let common = a
            .iter()
            .flat_map(|(f, p)| p.keys().map(move |t| (f, t)))
            .filter(|(f, t)| b.get(f).is_some_and(|p| p.contains_key(t)))
            .count();
        println!(
            "{name}: built {} paths, the file {}, {common} in both, {same_length} of the same length, {same} equal",
            count(a),
            count(b)
        );
        let mut longer = 0;
        for (from, paths) in a {
            for (to, path) in paths {
                assert_eq!(path.first(), Some(from));
                assert_eq!(path.last(), Some(to));
                if b.get(from)
                    .and_then(|p| p.get(to))
                    .is_some_and(|q| q.len() < path.len())
                {
                    longer += 1;
                }
            }
        }
        assert_eq!(longer, 0, "{name}: a built path is longer than the file's");
    }
}

/// StoryLauncher's constructor reads the specific NPCs from the world's
/// data root, and its destructor empties them.
#[test]
fn story_launcher_reads_the_specific_npcs_on_outdir() {
    let outdir = outdir();
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no content loaders");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let world_config = WorldConfig {
        single_player: true,
        ..WorldConfig::default()
    };
    let mut world = World::new(Arc::new(data), world_config)
        .with_assets(game_sim::object::world::Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let id = neuron_server::controller::ControllerId(5);
    let kind = game_logic::content::instantiate(
        &mut cx,
        id,
        b"StoryLauncher",
        &game_logic::server::CreateParams::None,
    )
    .expect("a Content kind");
    let npcs = Arc::clone(&cx.logic.content.state.specific_npcs);
    assert_eq!(npcs.len(), 107);
    assert_eq!(npcs[&id_of("juni")].individual_name, 219_800);
    game_logic::content::destroy_instance(&mut cx, id, kind);
    assert!(cx.logic.content.state.specific_npcs.is_empty());
}

fn group(cx: &Cx<'_>, name: &str) -> u32 {
    api::reputation::reputation_group(cx, name.as_bytes()).expect("group")
}

fn id_of(name: &str) -> u32 {
    create_id(name.as_bytes())
}

#[test]
fn the_point_bound_takes_the_zone_orientation_case() {
    // patrol_paths line 286 of the first CI run: y of a loop's ends, 8 ulps
    // apart but 4.5e-18 in value, because the zone's orientation element
    // [1][1] (about -2.8e-15) is the residue of a cancellation of sines.
    let oracle = "system=1 path=18 label=2 usage=1 eligible=1 loop=1 points=c729fb00:2d28de0b:462b8545 zones=21";
    let rust = "system=1 path=18 label=2 usage=1 eligible=1 loop=1 points=c729fb00:2d28de03:462b8545 zones=21";
    assert!(paths_within_bound(oracle, rust));
    // A difference outside the bound, or in any other field, is not.
    let far = "system=1 path=18 label=2 usage=1 eligible=1 loop=1 points=c729fb10:2d28de0b:462b8545 zones=21";
    assert!(!paths_within_bound(oracle, far));
    let other = "system=1 path=18 label=2 usage=1 eligible=1 loop=0 points=c729fb00:2d28de03:462b8545 zones=21";
    assert!(!paths_within_bound(oracle, other));
    assert!(within_bound(0x3f80_0000, 0x3f80_0004));
    assert!(!within_bound(0x3f80_0000, 0x3f80_0005));
}
