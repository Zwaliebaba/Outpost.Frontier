//! Content's static tables (work package P3.4b, decision record 0008
//! point 5): [`ContentTables::load`] over the original data, rendered as
//! `Content.dll`'s `OracleDumpContentTables` writes them
//! (`CppSource/Content/oracle_content_tables.cpp`), which `Oracle.exe sim`
//! runs for `CppSource/Oracle/content_tables.txt` on a multiplayer
//! start-up. The pilots' personalities are those `OracleSimPilotPersonality`
//! gives (`GetPilotPersonality`), compared field by field through
//! `Personality::from_bytes`, so the C++ padding is not compared.
//!
//! `data_dump_content_tables` reads `OUTPOST_DATA_DIR` (the dump root with
//! `EXE` and `DATA`) and `OUTPOST_ORACLE_DIR` (the oracle's output, with the
//! session's files in `content/`); without the first it prints a notice and
//! passes, without the second it renders and compares nothing. The other
//! tests check values of the shipped data in `outdir/` and return early
//! when it is missing. Nothing these tests read or write is committed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::content::tables::{ContentTables, files};
use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::ai::personality::Personality;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::costume::Costume;
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;

/// The tables in the oracle's order, each `content_<name>.txt`.
const TABLES: [&str; 17] = [
    "voice_lines",
    "voice_properties",
    "empathy",
    "formations",
    "pilots",
    "npc_ships",
    "factions",
    "faction_names",
    "faction_groups",
    "ship_classes",
    "ship_mission_properties",
    "loot",
    "phantom_loot",
    "gcs_exclusions",
    "toughness",
    "rank_difficulty",
    "news",
];

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn ids(values: &[u32]) -> String {
    values
        .iter()
        .map(u32::to_string)
        .collect::<Vec<_>>()
        .join(",")
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

fn names(list: &game_logic::content::tables::factions::NameList) -> String {
    let ranges: Vec<String> = list
        .ranges
        .iter()
        .map(|r| format!("{}-{}", r.first, r.last))
        .collect();
    format!("{}:{}", list.count, ranges.join(","))
}

fn fmt(f: &FmtStr) -> String {
    hex(&f.flatten().unwrap_or_default())
}

fn personality_hex(p: &Personality) -> String {
    hex(&p.to_bytes())
}

/// The lines of one table, as the export writes them.
fn render(t: &ContentTables, table: &str) -> Vec<String> {
    let mut out = Vec::new();
    match table {
        "voice_lines" => {
            for l in &t.voice_lines {
                out.push(format!(
                    "voice={} msg={} duration={}",
                    l.voice,
                    l.msg,
                    bits(l.duration)
                ));
            }
        }
        "voice_properties" => {
            for p in &t.voice_properties {
                let perms: Vec<String> = p
                    .permutation_counts
                    .iter()
                    .map(|(k, v)| format!("{k}:{v}"))
                    .collect();
                out.push(format!(
                    "voice={} flags={} permutations={}",
                    p.voice,
                    p.flags,
                    perms.join(",")
                ));
            }
        }
        "empathy" => {
            for (group, e) in &t.empathy {
                let rates: Vec<String> = e
                    .empathy_rates
                    .iter()
                    .map(|r| format!("{}:{}", r.group, bits(r.rate)))
                    .collect();
                let events: Vec<String> = e.event_rates.iter().map(|&r| bits(r)).collect();
                out.push(format!(
                    "group={group} events={} rates={}",
                    events.join(","),
                    rates.join(",")
                ));
            }
        }
        "formations" => {
            for (key, f) in &t.formations {
                let slots: Vec<String> = f
                    .slots
                    .iter()
                    .map(|s| {
                        format!(
                            "{}:{}:{}:{}",
                            bits(s.position[0]),
                            bits(s.position[1]),
                            bits(s.position[2]),
                            u32::from(s.player_slot)
                        )
                    })
                    .collect();
                out.push(format!(
                    "formation={key} nickname={} slots={}",
                    f.nickname,
                    slots.join(",")
                ));
            }
        }
        "pilots" => {
            let p = &t.pilots;
            for (&id, refs) in &p.pilots {
                let mut personality = Personality::default();
                let found = p.personality(id, &mut personality);
                out.push(format!(
                    "pilot={id} refs={} found={} personality={}",
                    ids(&refs.ids()),
                    u32::from(found),
                    personality_hex(&personality)
                ));
            }
            fn keys<T>(map: &BTreeMap<u32, T>) -> String {
                ids(&map.keys().copied().collect::<Vec<_>>())
            }
            for (kind, list) in [
                ("evade_dodge", keys(&p.evade_dodge)),
                ("evade_break", keys(&p.evade_break)),
                ("buzz_head_toward", keys(&p.buzz_head_toward)),
                ("buzz_pass_by", keys(&p.buzz_pass_by)),
                ("trail", keys(&p.trail)),
                ("strafe", keys(&p.strafe)),
                ("engine_kill", keys(&p.engine_kill)),
                ("repair", keys(&p.repair)),
                ("gun", keys(&p.gun)),
                ("mine", keys(&p.mine)),
                ("missile", keys(&p.missile)),
                ("damage_reaction", keys(&p.damage_reaction)),
                ("missile_reaction", keys(&p.missile_reaction)),
                ("countermeasure", keys(&p.countermeasure)),
                ("formation", keys(&p.formation)),
                ("job", keys(&p.job)),
            ] {
                out.push(format!("blocks={kind} ids={list}"));
            }
        }
        "npc_ships" => {
            for (key, a) in &t.npc_ships {
                out.push(format!(
                    "npc_ship={key} nickname={} ship={} loadout={} pilot={} state_graph={} level={} classes={}",
                    a.nickname,
                    a.ship_archetype,
                    a.loadout,
                    a.pilot,
                    a.state_graph,
                    a.level,
                    ids(&a.npc_classes)
                ));
            }
        }
        "factions" => {
            for (key, p) in &t.factions.props {
                let cargo: Vec<String> = p
                    .scan_for_cargo
                    .iter()
                    .map(|c| format!("{}:{}", c.archetype, c.level))
                    .collect();
                let formations: Vec<String> = p
                    .formations
                    .iter()
                    .map(|(k, v)| format!("{k}:{v}"))
                    .collect();
                let costumes: Vec<String> = p.space_costumes.iter().map(costume).collect();
                out.push(format!(
                    "faction={key} npc_ships={} jump={} scan_cargo={} scan_chance={} scan_announce={} \
market_goods={} assassinate={} legality={} singular={} formations={} costumes={} voices={} \
mc_costume={} msg_id_prefix={}",
                    ids(&p.npc_ships),
                    p.jump_preference,
                    cargo.join(","),
                    bits(p.scan_chance),
                    u32::from(p.scan_announce),
                    ids(&p.market_goods),
                    ids(&p.assassinate_targets),
                    p.legality,
                    u32::from(p.singular_nickname),
                    formations.join(","),
                    costumes.join(";"),
                    ids(&p.voices),
                    p.mc_costume,
                    hex(&p.msg_id_prefix)
                ));
            }
        }
        "faction_names" => {
            for (key, n) in &t.factions.names {
                out.push(format!(
                    "faction={key} group={} female={} male={} last={} formation={} large_ship={} \
large_ship_desig={} short_name={} rank_desig={} thresholds={},{} name_ids={}",
                    n.group,
                    names(&n.female_first_names),
                    names(&n.male_first_names),
                    names(&n.last_names),
                    names(&n.formation_desigs),
                    names(&n.large_ship_names),
                    n.large_ship_desig,
                    n.short_group_name,
                    ids(&n.rank_desig),
                    n.rank_threshold[0],
                    n.rank_threshold[1],
                    ids(&n.name_ids)
                ));
            }
        }
        "faction_groups" => {
            for g in &t.factions.groups {
                out.push(format!("group={g}"));
            }
        }
        "ship_classes" | "ship_mission_properties" => {
            let (key, map) = if table == "ship_classes" {
                ("class", &t.ship_classes)
            } else {
                ("ship", &t.ship_mission_properties)
            };
            for (id, members) in map {
                out.push(format!("{key}={id} members={}", ids(members)));
            }
        }
        "loot" => {
            for l in &t.loot_props {
                let props: Vec<String> = l.drop_properties.iter().map(i32::to_string).collect();
                out.push(format!(
                    "loot={} drop_chance={} drop_properties={}",
                    l.nickname,
                    bits(l.drop_chance),
                    props.join(",")
                ));
            }
        }
        "phantom_loot" => {
            for p in &t.phantom_loot {
                out.push(format!(
                    "phantom={} toughness={},{} chance={} num_to_drop={},{}",
                    p.nickname,
                    p.toughness_min,
                    p.toughness_max,
                    bits(p.percent_chance),
                    p.num_to_drop_min,
                    p.num_to_drop_max
                ));
            }
        }
        "gcs_exclusions" => {
            for (voice, scripts) in &t.gcs_exclusions {
                let scripts: Vec<String> = scripts.iter().map(|s| hex(s)).collect();
                out.push(format!("voice={voice} scripts={}", scripts.join(",")));
            }
        }
        "toughness" => {
            for p in &t.toughness_scale {
                out.push(format!(
                    "experience={} toughness={}",
                    p.experience, p.toughness
                ));
            }
        }
        "rank_difficulty" => {
            for r in &t.rank_difficulties {
                out.push(format!(
                    "stage={} difficulty={}",
                    r.stage,
                    bits(r.difficulty)
                ));
            }
        }
        "news" => {
            for (index, n) in t.news.iter().enumerate() {
                out.push(format!(
                    "news={index} bases={} from={} to={} audio={} logo={} breaking={} autoselect={} \
icon={} category={} headline={} text={}",
                    ids(&n.bases),
                    n.from_stage,
                    n.to_stage,
                    hex(&n.audio.to_ascii_lowercase()),
                    hex(&n.logo.to_ascii_lowercase()),
                    u32::from(n.breaking),
                    u32::from(n.autoselect),
                    n.icon,
                    fmt(&n.category),
                    fmt(&n.headline),
                    fmt(&n.text)
                ));
            }
        }
        _ => unreachable!("unknown table {table}"),
    }
    out
}

/// The tables loaded from a data root: the engine INI under `exe`, the data
/// under `data`; a multiplayer world, as the oracle's server is.
fn load(exe: &Path, data_dir: &Path) -> Option<(ContentTables, usize)> {
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let tables = ContentTables::load(&mut cx, &Host::new(data_dir));
    let reports = cx.logic.reports.0.len();
    for r in cx.logic.reports.0.iter().take(10) {
        println!("report: {r:?}");
    }
    Some((tables, reports))
}

fn outdir_tables() -> Option<ContentTables> {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    load(&outdir, &outdir.join("data")).map(|(t, _)| t)
}

/// An oracle pilot line with its personality decoded and encoded again, so
/// the padding the C++ copies is zero as in the port's.
fn normalise_pilot(line: &str) -> String {
    let Some((head, tail)) = line.split_once(" personality=") else {
        return line.to_owned();
    };
    let bytes: Option<Vec<u8>> = (0..tail.len() / 2)
        .map(|i| u8::from_str_radix(tail.get(2 * i..2 * i + 2)?, 16).ok())
        .collect();
    match bytes.as_deref().and_then(Personality::from_bytes) {
        Some(p) => format!("{head} personality={}", personality_hex(&p)),
        None => line.to_owned(),
    }
}

/// The differences of one table from the oracle's file.
fn compare(table: &str, oracle: &str, rendered: &[String]) -> Vec<String> {
    let mut problems = Vec::new();
    let want: Vec<String> = oracle
        .lines()
        .map(|l| {
            if table == "pilots" {
                normalise_pilot(l)
            } else {
                l.to_owned()
            }
        })
        .collect();
    if want.len() != rendered.len() {
        problems.push(format!(
            "{table}: rust has {} lines, the oracle {}",
            rendered.len(),
            want.len()
        ));
    }
    let cut = |s: &str| s.chars().take(300).collect::<String>();
    for (i, (w, g)) in want.iter().zip(rendered).enumerate() {
        if w != g {
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
fn data_dump_content_tables() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the content tables data dump was skipped");
        return;
    };
    let Some((tables, reports)) = load(&root.join("EXE"), &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let rendered: Vec<(&str, Vec<String>)> =
        TABLES.iter().map(|&t| (t, render(&tables, t))).collect();
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "content tables: {} tables, {} lines, {} load reports, {} reader reports",
        rendered.len(),
        rendered.iter().map(|(_, l)| l.len()).sum::<usize>(),
        reports,
        tables.ini_reports.len()
    );
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        print!("{summary}");
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let dir = oracle.join("content");
    let mut problems = Vec::new();
    for (table, lines) in &rendered {
        let _ = writeln!(summary, "  {table}: {} lines", lines.len());
        let path = dir.join(format!("content_{table}.txt"));
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
    for p in problems.iter().take(25) {
        let _ = writeln!(summary, "{p}");
    }
    print!("{summary}");
    let _ = std::fs::write(oracle.join("rust-content.txt"), &summary);
    assert!(
        problems.is_empty(),
        "content tables: {} differences from the oracle",
        problems.len()
    );
}

/// Every table renders, and its size is the shipped data's.
#[test]
fn content_tables_counts_on_outdir() {
    let Some(t) = outdir_tables() else {
        println!("outdir/ is missing: no content tables");
        return;
    };
    for table in TABLES {
        println!("{table}: {} lines", render(&t, table).len());
    }
    assert!(t.ini_reports.is_empty(), "{:?}", t.ini_reports);
    // 12,274 + 3,696 + 1,353 `[Sound]` sections, each with a duration.
    assert_eq!(t.voice_lines.len(), 17_323);
    // 48 distinct voices over 93 `[mVoiceProp]` sections.
    assert_eq!(t.voice_properties.len(), 48);
    assert_eq!(t.empathy.len(), 55);
    assert_eq!(t.formations.len(), 93);
    // 64 + 255 `[Pilot]`s, one story nickname twice.
    assert_eq!(t.pilots.pilots.len(), 318);
    assert_eq!(t.pilots.gun.len(), 99);
    // 45 + 14 `[JobBlock]`s; job_escort_a, prisoner_job and
    // story_job_gunboat_b are each defined twice.
    assert_eq!(t.pilots.job.len(), 56);
    assert_eq!(t.npc_ships.len(), 442);
    assert_eq!(t.factions.props.len(), 55);
    assert_eq!(t.factions.names.len(), 55);
    assert_eq!(t.factions.groups.len(), 55);
    assert_eq!(t.ship_classes.len(), 78);
    assert_eq!(t.ship_mission_properties.len(), 69);
    assert_eq!(t.loot_props.len(), 434);
    assert_eq!(t.phantom_loot.len(), 124);
    assert_eq!(t.gcs_exclusions.len(), 13);
    // 39 points, then a multiplayer server's 52 extrapolated ones.
    assert_eq!(t.toughness_scale.len(), 91);
    assert_eq!(t.rank_difficulties.len(), 9);
    assert_eq!(t.news.len(), 403);
}

/// One entry of each table, its fields read off the INI files.
#[test]
fn content_tables_known_values_on_outdir() {
    let Some(t) = outdir_tables() else {
        println!("outdir/ is missing: no content tables");
        return;
    };
    let id = |name: &str| create_id(name.as_bytes());
    let deg = |v: f64| (v * f64::from(0.017_453_292_f32)) as f32;
    let percent = |v: f64| (v * f64::from(0.01_f32)) as f32;
    // An INI value as `get_value_float` reads it and a float field keeps it.
    let f = |text: &str| text.parse::<f64>().map_or(f32::NAN, |v| v as f32);

    // AUDIO/voices_space_male.ini, the first [Sound] of atc_leg_m01.
    let line = t
        .voice_lines
        .iter()
        .find(|l| l.voice == id("atc_leg_m01") && l.msg == id("gcs_combat_scream_01-"))
        .expect("voice line");
    assert_eq!(line.duration, 3.516_644_f32);
    assert!(
        t.voice_lines
            .windows(2)
            .all(|w| (w[0].voice, w[0].msg) <= (w[1].voice, w[1].msg))
    );

    // atc_leg_m01: male, hail_dock, dockrequest_dock and combat_dock in the
    // voice file; MISSIONS/voice_properties.ini adds its permutations.
    let props = t
        .voice_properties
        .iter()
        .find(|p| p.voice == id("atc_leg_m01"))
        .expect("voice properties");
    assert_eq!(props.flags, 0x1 | 0x20 | 0x40 | 0x800);
    assert_eq!(props.permutation_count(id("gcs_misc_number")), 42);
    assert_eq!(props.permutation_count(id("gcs_dockrequest_proceed")), 1);

    // MISSIONS/empathy.ini, he_n_grp's events and its largest rate.
    let empathy = t
        .empathy
        .values()
        .find(|e| {
            e.event_rates
                == [
                    f("-0.0299999993"),
                    f("0.0859000012"),
                    f("-0.0450000018"),
                    f("-0.0675000027"),
                ]
        })
        .expect("he_n_grp empathy");
    assert!(
        empathy
            .empathy_rates
            .windows(2)
            .all(|w| w[0].rate <= w[1].rate)
    );
    assert_eq!(
        empathy.empathy_rates.last().map(|r| r.rate),
        Some(f("0.400000006"))
    );

    // MISSIONS/formations.ini, the first [Formation].
    let chase = t.formation(id("story_chase")).expect("story_chase");
    assert_eq!(chase.nickname, id("story_chase"));
    let slots: Vec<([f32; 3], bool)> = chase
        .slots
        .iter()
        .map(|s| (s.position, s.player_slot))
        .collect();
    assert_eq!(
        slots,
        [
            ([0.0, 0.0, 0.0], false),
            ([-19.0, 10.0, 203.0], false),
            ([30.0, 20.0, 240.0], false)
        ]
    );
    let single_r = t.formation(id("single_r")).expect("single_r");
    assert_eq!((single_r.ship_slots(), single_r.player_slots()), (1, 1));

    // MISSIONS/pilots_population.ini, the pilot `test` and its blocks.
    let mut p = Personality::default();
    assert!(t.pilots.personality(id("test"), &mut p));
    assert_eq!(p.gun.fire_interval_time, f("0.100000001"));
    assert_eq!(p.gun.fire_accuracy_cone_angle, deg(3.0));
    assert_eq!(p.gun.fire_style, 0);
    assert_eq!(p.gun.range_threshold, f("1.10000002"));
    assert_eq!(p.gun.fire_accuracy_power_npc, 3.0);
    assert_eq!(p.evade_dodge.cone_angle, deg(90.0));
    assert_eq!(p.evade_dodge.style_weight_corkscrew, 1.0);
    assert_eq!(p.evade_dodge.evade_activate_range, 250.0);
    assert_eq!(p.evade_dodge.direction_weight_left, 0.5);
    assert_eq!(p.evade_dodge.corkscrew_roll_flip_direction, 0);
    // job_test.
    assert_eq!(p.job.wait_for_leader_target, 1);
    assert_eq!(p.job.maximum_leader_target_distance, 2500.0);
    assert_eq!(p.job.flee_when_leader_flees_style, 0);
    assert_eq!(p.job.scene_toughness_threshold, 4);
    assert_eq!(p.job.loot_flee_threshold, 4);
    assert_eq!(p.job.field_targeting, 0);
    assert_eq!(&p.job.attack_subtarget_order[..2], &[6, 7]);
    assert_eq!(
        (
            p.job.force_attack_formation,
            p.job.force_attack_formation_set
        ),
        (1, 1)
    );
    // `lt_potions` in lower case: the reconstruction's case-sensitive test
    // gives 0 (decision record 0008, open question 4).
    assert_eq!(p.job.loot_preference, 0);
    let classes: Vec<(i32, f32, u32)> = p
        .job
        .attack_preferences
        .iter()
        .take(10)
        .map(|a| (a.ship_class, a.range, a.weapon_flags))
        .collect();
    let mut want: Vec<(i32, f32, u32)> = [0, 1, 2, 3, 4, 5, 8, 10, 11]
        .iter()
        .map(|&c| (c, 5000.0, 7))
        .collect();
    want.push((12, 0.0, 0));
    assert_eq!(classes, want);
    // No job of the shipped data loots.
    assert!(t.pilots.job.values().all(|j| j.loot_preference == 0));

    // MISSIONS/pilots_story.ini: story_light_fighter_easy inherits from
    // story_light_fighter_default and replaces its gun.
    let base = t.pilots.pilots[&id("story_light_fighter_default")];
    let easy = t.pilots.pilots[&id("story_light_fighter_easy")];
    assert_eq!(easy.inherit, id("story_light_fighter_default"));
    assert_eq!(easy.gun, id("gun_story_fighter_easy_style_a"));
    assert_eq!(base.job, id("story_job_fighter_a"));
    assert_eq!(easy.job, base.job);
    assert_eq!(easy.evade_dodge, id("evade_dodge_fighter_chaos_med"));
    assert!(!t.pilots.personality(id("no_such_pilot"), &mut p));

    // MISSIONS/npcships.ini, the first [NPCShipArch].
    let rogue = t.npc_ship(id("MSN01a_Helion_Rogue")).expect("npc ship");
    assert_eq!(rogue.ship_archetype, id("pi_fighter"));
    assert_eq!(rogue.loadout, id("MSN01a_Helion_Rogue"));
    assert_eq!(rogue.pilot, id("MSN01a_Helion_Rogue"));
    assert_eq!(rogue.level, 1);
    assert!(rogue.state_graph >= 0);
    assert_eq!(rogue.npc_classes, [id("unlawful"), id("FIGHTER")]);

    // MISSIONS/faction_prop.ini, the first [FactionProps] (he_n_grp).
    let (&he_n, props) = t
        .factions
        .props
        .iter()
        .find(|(_, p)| p.npc_ships.first() == Some(&id("li_n_he_elite_d1-3")))
        .expect("he_n_grp props");
    assert_eq!(props.msg_id_prefix, b"gcs_refer_faction_li_n");
    assert_ne!(he_n, u32::MAX);
    assert_eq!(props.legality, 1);
    assert!(props.singular_nickname);
    assert_eq!(props.jump_preference, 1);
    assert_eq!(props.npc_ships.len(), 11);
    assert_eq!(props.npc_ships[0], id("li_n_he_elite_d1-3"));
    assert_eq!(
        props.voices,
        [
            id("pilot_f_mil_m01"),
            id("pilot_f_mil_m02"),
            id("pilot_f_leg_f01"),
            id("pilot_f_leg_f01a")
        ]
    );
    assert_eq!(props.space_costumes.len(), 12);
    assert_eq!(props.space_costumes[0].accessory_ids.len(), 1);
    assert_eq!(props.scan_for_cargo.len(), 2);
    assert_eq!(
        (props.scan_for_cargo[0].level, props.scan_for_cargo[1].level),
        (1, 2)
    );
    assert!(props.scan_announce);
    assert_eq!(props.scan_chance, f("0.300000012"));
    assert_eq!(props.formation(id("fighters")), Some(id("fighter_li_n")));
    assert!(props.market_goods.is_empty());
    let names = t.faction_names(he_n).expect("he_n_grp names");
    assert_eq!(names.group, he_n);
    assert_eq!(names.male_first_names.count, 226_741 - 226_608 + 1);
    assert_eq!(names.female_first_names.count, 226_952 - 226_808 + 1);
    assert_eq!(names.last_names.count, 227_307 - 227_008 + 1);
    assert_eq!(names.formation_desigs.count, 197_820 - 197_808 + 1);
    assert_eq!(names.large_ship_names.count, 202_647 - 202_608 + 1);
    assert_eq!(names.large_ship_desig, 196_976);
    assert_eq!(names.rank_desig, [197_002, 197_003, 197_004]);
    assert_eq!(names.rank_threshold, [6, 9]);
    assert_eq!(names.name_ids, [0, 0, 0x4076, 0x4074, 0x4074, 0, 0, 0x4074]);
    assert_eq!(names.male_first_names.name(1), Some(226_609));
    // EQUIPMENT/commodities_per_faction.ini: co_shi_grp's 30 goods.
    assert!(
        t.factions
            .props
            .values()
            .any(|p| p.market_goods.len() == 30)
    );
    let mut groups = t.factions.groups.clone();
    groups.sort_unstable();
    assert_eq!(groups, t.factions.groups);

    // MISSIONS/shipclasses.ini: SMALL_SHIPS expands fighters and freighters.
    assert_eq!(
        t.ship_class(id("fighters")),
        Some(&[id("elite_fighter"), id("fighter")][..])
    );
    assert_eq!(
        t.ship_class(id("SMALL_SHIPS")),
        Some(
            &[
                id("elite_fighter"),
                id("fighter"),
                id("freighter"),
                id("armored"),
                id("csv")
            ][..]
        )
    );
    // SHIPS/shiparch.ini: he_fighter can use berths.
    assert_eq!(
        t.ship_mission_properties.get(&id("he_fighter")),
        Some(&vec![id("can_use_berths")])
    );

    // MISSIONS/lootprops.ini.
    let commodity = &t.loot_props[0];
    assert_eq!(commodity.nickname, id("default_commodity"));
    assert_eq!(commodity.drop_chance, 0.0);
    assert_eq!(commodity.drop_properties, [100_000, 10_000, 1, 2, 1]);
    let battery = &t.loot_props[2];
    assert_eq!(battery.nickname, id("ge_s_battery_01"));
    assert_eq!(battery.drop_chance, percent(33.0));
    assert_eq!(battery.drop_properties, [0, 1, 0, 1000, 1000]);
    let repair = &t.phantom_loot[0];
    assert_eq!(repair.nickname, id("ge_s_repair_01"));
    assert_eq!((repair.toughness_min, repair.toughness_max), (0, 1));
    assert_eq!(repair.percent_chance, percent(16.0));
    assert_eq!((repair.num_to_drop_min, repair.num_to_drop_max), (1, 1));

    // SCRIPTS/GCS/gcsexcl.ini: rvp101's one script.
    assert_eq!(
        t.gcs_exclusions.get(&id("rvp101")),
        Some(&vec![b"scripts\\gcs\\GCS_MLStand_F01aB_01.thn".to_vec()])
    );

    // MISSIONS/ptough.ini: 0, 0 is raised to rank 1; the last point is
    // 2409599, 38; the first extrapolated point adds an eighth.
    let scale = &t.toughness_scale;
    assert_eq!((scale[0].experience, scale[0].toughness), (0, 1));
    assert_eq!((scale[1].experience, scale[1].toughness), (11_000, 1));
    assert_eq!((scale[38].experience, scale[38].toughness), (2_409_599, 38));
    assert_eq!(
        (scale[39].experience, scale[39].toughness),
        (2_409_599 + 2_409_599 / 8, 39)
    );
    assert!(scale.last().is_some_and(|p| p.experience >= 999_999_999));

    // MISSIONS/rankdiff.ini, sorted by stage.
    let ranks = &t.rank_difficulties;
    assert_eq!(
        (ranks[0].stage, ranks[0].difficulty),
        (0, f("0.0820999965"))
    );
    assert_eq!((ranks[8].stage, ranks[8].difficulty), (0x2a, 38.983_86));
    assert!(ranks.windows(2).all(|w| w[0].stage <= w[1].stage));

    // MISSIONS/news.ini, the first [NewsItem].
    let news = &t.news[0];
    assert_eq!((news.from_stage, news.to_stage), (1, 1));
    assert!(news.autoselect);
    assert!(!news.breaking);
    assert_eq!(news.icon, 1);
    assert_eq!(news.logo, b"news_scene2");
    assert_eq!(news.category.mad_lib_id, 15_001);
    assert_eq!(news.headline.mad_lib_id, 15_001);
    assert_eq!(news.text.mad_lib_id, 15_002);
    assert!(news.text.values.is_empty());
    assert_eq!(news.bases.len(), 1);
    assert_ne!(news.bases[0], 0);
}

/// A single-player world loads the same tables, without the multiplayer
/// extrapolation of the toughness scale, from the world's data root.
#[test]
fn content_tables_single_player_on_outdir() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no content tables");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let world_config = WorldConfig {
        single_player: true,
        ..WorldConfig::default()
    };
    let mut world =
        World::new(Arc::new(data), world_config).with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let t = ContentTables::load_from_world(&mut cx);
    assert_eq!(t.toughness_scale.len(), 39);
    assert_eq!(t.voice_lines.len(), 17_323);
    assert_eq!(t.news.len(), 403);
}

/// Without the files every table is empty: each loader runs over a closed
/// reader, as the C++'s untested opens make it.
#[test]
fn content_tables_without_files() {
    let empty = std::env::temp_dir().join("outpost-content-tables-none");
    let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let t = ContentTables::load(&mut cx, &Host::new(&empty));
    assert_eq!(t, ContentTables::default());
    assert_eq!(files::NEWS, b"missions\\news.ini");
}
