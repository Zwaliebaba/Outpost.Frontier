//! Oracle vectors of work packages P2.1 and P2.4: asteroid ids and cube orientations,
//! the string cache, the faction registry, the zone queries, the exclusion
//! zones, the spatial partitions with the system zone tree, the base
//! records' marshalling, and the object-model math (`object_math`:
//! `LookMatrix`, `ComputeTimeToTgt`, the nudge and axis setters). The inputs are under
//! `tests/fixtures/oracle/`, written by `make_game_sim_fixtures.py`; the C++
//! side is `CppSource/Oracle/oracle_game_suites.cpp`, run as
//! `Oracle.exe game-sim <fixtures> <output>`.
//!
//! `CAsteroidCube::compute_asteroid_id` is private in the C++ and has no
//! export the harness can call; its unit tests are in the crate.
//!
//! Every suite is Tier A and compared as text except `asteroid_orient`,
//! whose matrix went through sine and cosine, and `zones` and
//! `zone_queries`, whose rotated zones did (`EulerMatrix`); their floats are
//! compared within a bound, the answers that are not floats exactly. The
//! frustum culls and queries take sines, cosines and tangents too; their
//! answers are compared exactly all the same.

mod support;

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use game_sim::data::Reports;
use game_sim::data::archetype::ArchetypeDb;
use game_sim::data::asteroid::{
    AxisRotations, closest_cube_coord, compute_cube_id, compute_cube_orientation,
};
use game_sim::data::reputation::{Relation, Reputation};
use game_sim::data::spatial::{
    QueryResult, SpatialEntry, SpatialPartition, SpatialRay, new_spatial,
};
use game_sim::data::string_cache::{CacheString, StringCache};
use game_sim::data::universe::{Universe, UniverseCx};
use game_sim::data::vibe::VibeDb;
use game_sim::data::zone::exclusion::{
    AsteroidExclusionZone, AsteroidExclusionZoneMgr, ExcludeObjectType, NebulaExclusionZoneMgr,
};
use game_sim::data::zone::query::{Frustum, Sphere};
use game_sim::protocol::base_props::{
    AmbientScriptProperties, CharacterProperties, MissionVendorAcceptance, MissionVendorProperties,
    NewsBroadcastProperties,
};
use game_sim::protocol::costume::Costume;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::marshal::{MarshalReader, MarshalWriter};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::vfs::Host;

use support::data::Tolerance;
use support::render::Line;
use support::{check_suite, f32_hex, f32_text, fixtures, hex, input_lines, read_fixture};

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

#[test]
fn asteroid_ids() {
    let mut lines = Vec::new();
    for line in input_lines("asteroid.list") {
        let v: Vec<f32> = line.split(' ').map(f32_hex).collect();
        let pos = [v[0], v[1], v[2]];
        lines.push(
            Line::new()
                .add("pos", line.replace(' ', ","))
                .add("cube", format!("{:08x}", compute_cube_id(pos)))
                .done(),
        );
    }
    for coord in [
        -1000,
        -300,
        -256,
        -1,
        0,
        1,
        255,
        256,
        300,
        1000,
        i32::MAX,
        i32::MIN,
    ] {
        for size in [256, 1000, 1, -256] {
            lines.push(
                Line::new()
                    .add("coord", coord.to_string())
                    .add("size", size.to_string())
                    .add(
                        "closest",
                        closest_cube_coord(coord, size, size / 2).to_string(),
                    )
                    .done(),
            );
        }
    }
    check_suite("asteroid_ids.jsonl", &lines);
}

/// Tier B: the matrix within 4 ulps or 1e-6 (entries that cancel to near
/// zero).
#[test]
fn asteroid_orient() {
    let mut lines = Vec::new();
    for line in input_lines("orient.list") {
        let parts: Vec<&str> = line.split(' ').collect();
        let seed: u32 = parts[0].parse().unwrap_or_else(|e| panic!("{line}: {e}"));
        let freq = f32_hex(parts[1]);
        let mut rotations = AxisRotations::default();
        for (i, word) in parts[2..14].iter().enumerate() {
            rotations.angles[i / 4][i % 4] = f32_hex(word);
        }
        let mut m = Matrix::IDENTITY;
        let filled = compute_cube_orientation(seed, &mut m, &rotations, freq);
        let words: Vec<String> = m.to_words().iter().map(|&w| f32_text(w)).collect();
        lines.push(
            Line::new()
                .add("seed", seed.to_string())
                .add("freq", parts[1])
                .add("filled", u8::from(filled).to_string())
                .add("m", words.join(","))
                .done(),
        );
    }
    let name = "asteroid_orient.jsonl";
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, &lines);
        return;
    }
    let tolerance = Tolerance {
        keys: &["m"],
        ulps: 4,
        absolute: 1.0e-6,
    };
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  oracle: {want}\n  rust:   {got}",
                index + 1
            );
        }
    }
}

/// Compares a Tier B suite with the committed file, or writes it.
fn check_within(name: &str, lines: &[String], tolerance: &Tolerance) {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, lines);
        return;
    }
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  oracle: {want}\n  rust:   {got}",
                index + 1
            );
        }
    }
}

fn vec3_text(v: Vector) -> String {
    format!("{},{},{}", f32_text(v.x), f32_text(v.y), f32_text(v.z))
}

/// Tier B: distances, surface points and radii within 4 ulps or 1e-3.
#[test]
fn zones() {
    let source = Host::new(fixtures());
    let mut strings = StringCache::new();
    let mut reports = Reports::default();
    let archetypes = ArchetypeDb::default();
    let mut universe = Universe::default();
    let reputation = Reputation::default();
    let mut vibes = VibeDb::default();
    let mut cx = UniverseCx {
        strings: &mut strings,
        archetypes: &archetypes,
        reputation: &reputation,
        vibes: &mut vibes,
        reports: &mut reports,
    };
    universe.startup(&source, b"zones\\universe.ini", &mut cx);
    let mut points = Vec::new();
    let mut directions = Vec::new();
    for line in input_lines("zone_points.list") {
        let (list, text) = match line.strip_prefix("d ") {
            Some(rest) => (&mut directions, rest.to_owned()),
            None => (&mut points, line.clone()),
        };
        let v: Vec<f32> = text.split(' ').map(f32_hex).collect();
        list.push((
            text.replace(' ', ","),
            Vector {
                x: v[0],
                y: v[1],
                z: v[2],
            },
        ));
    }
    let mut lines = Vec::new();
    for zone in universe.system_zones(neuron_core::id::create_id(b"zt")) {
        lines.push(
            Line::new()
                .add("zone", format!("{:08x}", zone.nickname))
                .add("shape", zone.shape.value().to_string())
                .add("radius", f32_text(zone.radius()))
                .done(),
        );
        for (text, p) in &points {
            lines.push(
                Line::new()
                    .add("p", text.clone())
                    .add(
                        "in",
                        format!(
                            "{},{}",
                            u8::from(zone.in_zone(*p, 0.0)),
                            u8::from(zone.in_zone(*p, 10.0))
                        ),
                    )
                    .add("in_xz", u8::from(zone.in_zone_xz(*p, 0.0)).to_string())
                    .add("dist", f32_text(zone.distance(*p) as f32))
                    .add("surface", vec3_text(zone.surface_point(*p)))
                    .done(),
            );
        }
        for (text, d) in &directions {
            lines.push(
                Line::new()
                    .add("d", text.clone())
                    .add("along", f32_text(zone.radius_along(*d) as f32))
                    .done(),
            );
        }
    }
    let tolerance = Tolerance {
        keys: &["radius", "dist", "surface", "along"],
        ulps: 4,
        absolute: 1.0e-3,
    };
    check_within("zones.jsonl", &lines, &tolerance);
}

#[test]
fn string_cache() {
    let mut cache = StringCache::new();
    let mut first: HashMap<CacheString, usize> = HashMap::new();
    let mut lines = Vec::new();
    for (index, line) in input_lines("strings.list").iter().enumerate() {
        let (op, arg) = line.split_once(' ').unwrap_or((line.as_str(), ""));
        let result = match op {
            "i" => Some(cache.alloc(&unhex(arg), false)),
            "t" => Some(cache.alloc(&unhex(arg), true)),
            "bay" => Some(cache.cargo_bay()),
            _ => {
                cache.shutdown();
                None
            }
        };
        let mut out = Line::new();
        out.add("op", op).add("in", arg);
        if let Some(handle) = result {
            let seen = if op == "bay" {
                first
                    .get(&handle)
                    .map_or(-1, |&i| i64::try_from(i).unwrap_or(-1))
            } else {
                i64::try_from(*first.entry(handle).or_insert(index)).unwrap_or(-1)
            };
            out.add("out", hex(cache.get(handle)))
                .add("first", seen.to_string());
        }
        lines.push(out.done());
    }
    check_suite("string_cache.jsonl", &lines);
}

fn name_arg(text: &str) -> Vec<u8> {
    if text == "-" { Vec::new() } else { unhex(text) }
}

fn feeling(rep: &Reputation, from: u32, to: u32) -> String {
    let (result, value) = rep.feelings_towards(from, to);
    format!("{result},{}", value.map_or("-".to_owned(), f32_text))
}

#[test]
fn reputation() {
    let source = Host::new(fixtures());
    let mut rep = Reputation::default();
    let mut lines = Vec::new();
    let started = rep.startup(&source, b"groups.ini");
    lines.push(
        Line::new()
            .add("startup", u8::from(started).to_string())
            .add("count", rep.group_count().to_string())
            .done(),
    );
    let ids: Vec<u32> = rep.groups().map(|(id, _)| id).collect();
    for &id in &ids {
        lines.push(
            Line::new()
                .add("group", format!("{id:04x}"))
                .add("nickname", hex(rep.get_nickname(id).unwrap_or_default()))
                .add("name", rep.get_name(id).to_string())
                .add("info", rep.get_info_card(id).to_string())
                .add("short", rep.get_short_name(id).to_string())
                .done(),
        );
    }
    let mut reader = IniReader::new();
    assert!(reader.open(b"feelings.ini", read_fixture("feelings.ini")));
    let mut reports = Reports::default();
    rep.load_feelings(&mut reader, &mut reports);
    for &from in &ids {
        for &to in &ids {
            lines.push(
                Line::new()
                    .add("from", format!("{from:04x}"))
                    .add("to", format!("{to:04x}"))
                    .add("feel", feeling(&rep, from, to))
                    .done(),
            );
        }
    }
    for line in input_lines("reputation.list") {
        let parts: Vec<&str> = line.split(' ').collect();
        let mut out = Line::new();
        out.add("op", parts[0]);
        match parts[0] {
            "id" => {
                out.add("group", format!("{:08x}", rep.get_id(&name_arg(parts[1]))));
            }
            "feel" => {
                let from = rep.get_id(&name_arg(parts[1]));
                let to = rep.get_id(&name_arg(parts[2]));
                out.add("feel", feeling(&rep, from, to));
            }
            "affect" => {
                let from = rep.get_id(&name_arg(parts[1]));
                let to = rep.get_id(&name_arg(parts[2]));
                let result = rep.affect_relation(from, to, f32_hex(parts[3]));
                out.add("result", result.to_string())
                    .add("feel", feeling(&rep, from, to));
            }
            "story" => {
                let id = game_sim::data::reputation::group_id(&name_arg(parts[1]));
                out.add(
                    "story",
                    u8::from(Reputation::is_story_faction(id)).to_string(),
                );
            }
            _ => {
                let relations: Vec<Relation> = parts[2..]
                    .iter()
                    .map(|pair| {
                        let (name, value) = pair.split_once(':').unwrap_or((pair, "0"));
                        Relation {
                            group_id: game_sim::data::reputation::group_id(&name_arg(name)),
                            feeling: f32_hex(value),
                        }
                    })
                    .collect();
                let name = name_arg(parts[1]);
                rep.set(&name, &relations);
                let id = game_sim::data::reputation::group_id(&name);
                let feelings: Vec<String> = rep
                    .groups()
                    .map(|(to, _)| format!("{to:04x}:{}", feeling(&rep, id, to)))
                    .collect();
                out.add("group", format!("{id:04x}"))
                    .add("nickname", hex(rep.get_nickname(id).unwrap_or_default()))
                    .add("feelings", feelings.join(";"));
            }
        }
        lines.push(out.done());
    }
    check_suite("reputation.jsonl", &lines);
}

/// The universe of `zone_queries/` (system `zq`).
fn query_universe() -> Universe {
    let source = Host::new(fixtures());
    let mut strings = StringCache::new();
    let mut reports = Reports::default();
    let archetypes = ArchetypeDb::default();
    let reputation = Reputation::default();
    let mut vibes = VibeDb::default();
    let mut universe = Universe::default();
    let mut cx = UniverseCx {
        strings: &mut strings,
        archetypes: &archetypes,
        reputation: &reputation,
        vibes: &mut vibes,
        reports: &mut reports,
    };
    universe.startup(&source, b"zone_queries\\universe.ini", &mut cx);
    universe
}

/// The words of every line of a fixture list, as floats.
fn float_rows(name: &str) -> Vec<Vec<f32>> {
    input_lines(name)
        .iter()
        .map(|l| l.split(' ').map(f32_hex).collect())
        .collect()
}

fn vec3(w: &[f32]) -> Vector {
    Vector {
        x: w[0],
        y: w[1],
        z: w[2],
    }
}

fn bits(answers: impl Iterator<Item = bool>) -> String {
    answers.map(|b| if b { '1' } else { '0' }).collect()
}

/// Tier A for the clips and classifications of unrotated zones, Tier B
/// for rotated zones (`EulerMatrix`) and the frustum cull (`FSIN`,
/// `FCOS`, `FPTAN`): floats within a bound, answers exact.
#[test]
fn zone_queries() {
    let universe = query_universe();
    let points: Vec<Vector> = input_lines("zone_points.list")
        .iter()
        .filter(|l| !l.starts_with("d "))
        .map(|l| vec3(&l.split(' ').map(f32_hex).collect::<Vec<_>>()))
        .collect();
    let segments = float_rows("zone_segments.list");
    let frustums: Vec<Frustum> = float_rows("zone_frustums.list")
        .iter()
        .map(|w| Frustum {
            xform: Transform::new(
                Matrix::from_rows([[w[0], w[1], w[2]], [w[3], w[4], w[5]], [w[6], w[7], w[8]]]),
                vec3(&w[9..12]),
            ),
            angle_x: w[12],
            angle_y: w[13],
            distance: w[14],
        })
        .collect();
    let spheres: Vec<Sphere> = float_rows("zone_spheres.list")
        .iter()
        .map(|w| Sphere {
            center: vec3(w),
            radius: w[3],
        })
        .collect();
    let mut reports = Reports::default();
    let mut lines = Vec::new();
    for zone in universe.system_zones(neuron_core::id::create_id(b"zq")) {
        lines.push(
            Line::new()
                .add("zone", format!("{:08x}", zone.nickname))
                .add("shape", zone.shape.value().to_string())
                .done(),
        );
        for (index, p) in points.iter().enumerate() {
            lines.push(
                Line::new()
                    .add("p", index.to_string())
                    .add("classify", f32_text(zone.classify_point(*p) as f32))
                    .add("immersion", f32_text(zone.immersion(*p) as f32))
                    .done(),
            );
        }
        for (index, s) in segments.iter().enumerate() {
            let mut line = Line::new();
            line.add("seg", index.to_string());
            match zone.intersect(vec3(s), vec3(&s[3..]), &mut reports) {
                Some(at) => line.add("hit", "1").add("at", vec3_text(at)),
                None => line.add("hit", "0"),
            };
            lines.push(line.done());
        }
        lines.push(
            Line::new()
                .add(
                    "frustums",
                    bits(frustums.iter().map(|f| zone.frustum_cull(f))),
                )
                .done(),
        );
        lines.push(
            Line::new()
                .add("spheres", bits(spheres.iter().map(|s| zone.sphere_cull(s))))
                .done(),
        );
    }
    let tolerance = Tolerance {
        keys: &["classify", "immersion", "at"],
        ulps: 4,
        absolute: 1.0e-3,
    };
    check_within("zone_queries.jsonl", &lines, &tolerance);
}

/// The exclusion zones' properties per nickname: billboards, dynamic
/// asteroids, empty cube frequency, billboard count.
const EXCLUSIONS: [(&[u8], bool, bool, f32, f32); 5] = [
    (b"x_a", true, false, -1.0, -1.0),
    (b"x_b", false, true, 0.5, 30.0),
    (b"x_c", false, false, -1.0, 12.7),
    (b"x_far", false, false, 0.25, -1.0),
    (b"x_edge", false, true, -1.0, 45.2),
];

/// The value `is_excluded` finds in its output before each call.
const UNTOUCHED: f32 = 12345.0;

fn exclusion_field(
    mgr: &AsteroidExclusionZoneMgr,
    ids: &[u32],
    get: impl Fn(&AsteroidExclusionZone) -> f32,
) -> String {
    ids.iter()
        .map(|id| {
            let z = mgr
                .zones
                .iter()
                .find(|z| z.zone.as_ref().map(|z| z.nickname) == Some(*id))
                .expect("zone");
            f32_text(get(z))
        })
        .collect::<Vec<_>>()
        .join(",")
}

/// The asteroid and nebula exclusion managers on the zones of `zq`; the
/// zones are not rotated, so everything is Tier A.
#[test]
fn exclusion_zones() {
    let universe = query_universe();
    let zone = |name: &[u8]| &universe.zones[&neuron_core::id::create_id(name)];
    let mut observers: Vec<(Vector, Vec<Vector>)> = Vec::new();
    for line in input_lines("exclusion.list") {
        let (kind, rest) = line.split_at(2);
        let p = vec3(&rest.split(' ').map(f32_hex).collect::<Vec<_>>());
        if kind == "o " {
            observers.push((p, Vec::new()));
        } else if let Some(last) = observers.last_mut() {
            last.1.push(p);
        }
    }
    let mut lines = Vec::new();
    let mut reports = Reports::default();

    let parent = zone(b"q_sphere");
    let mut mgr = AsteroidExclusionZoneMgr::default();
    let ids: Vec<u32> = EXCLUSIONS
        .iter()
        .map(|e| neuron_core::id::create_id(e.0))
        .collect();
    for &(name, billboards, dynamic, empty, count) in &EXCLUSIONS {
        mgr.push_back(Some(zone(name)), parent);
        let back = mgr.back().expect("pushed");
        let inside = back.set_zone(Some(zone(name)), parent);
        back.exclude_billboards = billboards;
        back.exclude_dynamic_asteroids = dynamic;
        back.empty_cube_frequency = empty;
        back.billboard_count = count;
        lines.push(
            Line::new()
                .add("push", format!("{:08x}", zone(name).nickname))
                .add("inside", u8::from(inside).to_string())
                .done(),
        );
    }
    let max = mgr.max_billboard_count();
    lines.push(Line::new().add("max_billboards", max.to_string()).done());
    if max > 0 {
        mgr.normalize_billboard_density(max);
    }
    lines.push(
        Line::new()
            .add(
                "densities",
                exclusion_field(&mgr, &ids, |z| z.billboard_count),
            )
            .done(),
    );
    for pass in 0..2 {
        if pass == 1 {
            mgr.set_max_cliasteroid_exclusion_distance(300.0);
            mgr.set_max_dynamicasteroid_exclusion_distance(100.0);
        }
        for (observer, queries) in &observers {
            mgr.sort(*observer);
            let back = mgr
                .back()
                .and_then(|z| z.zone.as_ref())
                .map_or(0, |z| z.nickname);
            lines.push(
                Line::new()
                    .add("observer", vec3_text(*observer))
                    .add("dist", exclusion_field(&mgr, &ids, |z| z.max_dist_sq))
                    .add("back", format!("{back:08x}"))
                    .done(),
            );
            for q in queries {
                let mut answers = Vec::new();
                let mut outs = Vec::new();
                for kind in 0..5 {
                    let mut out = UNTOUCHED;
                    let hit = mgr.is_excluded(*q, ExcludeObjectType::from_value(kind), &mut out);
                    answers.push(u8::from(hit).to_string());
                    outs.push(f32_text(out));
                }
                lines.push(
                    Line::new()
                        .add("q", vec3_text(*q))
                        .add("ex", answers.join(","))
                        .add("out", outs.join(","))
                        .done(),
                );
            }
        }
    }

    let nebula = zone(b"q_ellipsoid");
    let mut neb = NebulaExclusionZoneMgr::default();
    let names: [&[u8]; 3] = [b"x_d", b"x_a", b"x_b"];
    for (index, name) in names.iter().enumerate() {
        neb.push_back(zone(name), nebula, &mut reports);
        let back = neb.back().expect("pushed");
        if index == 0 {
            back.set_zone_shell_name(b"nebula_shell");
            back.fog_far = 1500.0;
        }
        lines.push(
            Line::new()
                .add("nebula_push", format!("{:08x}", zone(name).nickname))
                .add("shell", format!("{:08x}", back.zone_shell_name))
                .add("fog", f32_text(back.fog_far))
                .done(),
        );
    }
    let neb_ids: Vec<u32> = names.iter().map(|n| zone(n).nickname).collect();
    for (observer, queries) in &observers {
        neb.sort(*observer);
        let dist: Vec<String> = neb_ids
            .iter()
            .map(|id| {
                let z = neb
                    .zones
                    .iter()
                    .find(|z| z.zone.nickname == *id)
                    .expect("zone");
                f32_text(z.max_dist_sq)
            })
            .collect();
        lines.push(
            Line::new()
                .add("nebula_observer", vec3_text(*observer))
                .add("dist", dist.join(","))
                .done(),
        );
        let found = |near: bool, q: Vector| match neb.is_in_exclusion_zone(q, near) {
            Some((index, value)) => {
                format!("{:08x},{}", neb.zones[index].zone.nickname, f32_text(value))
            }
            None => format!("-,{}", f32_text(0.0)),
        };
        for q in queries {
            lines.push(
                Line::new()
                    .add("q", vec3_text(*q))
                    .add(
                        "excluded",
                        format!(
                            "{},{}",
                            u8::from(neb.is_excluded(*q, ExcludeObjectType::Nebula)),
                            u8::from(neb.is_excluded(*q, ExcludeObjectType::CliAsteroid))
                        ),
                    )
                    .add("in", found(false, *q))
                    .add("in_near", found(true, *q))
                    .done(),
            );
        }
    }
    check_suite("exclusion_zones.jsonl", &lines);
}

fn items_line(op: &str, count: usize, result: &QueryResult) -> String {
    let items: Vec<String> = result.items.iter().map(u32::to_string).collect();
    Line::new()
        .add("op", op)
        .add("n", count.to_string())
        .add("items", items.join(","))
        .done()
}

fn zone_text(zone: Option<u32>) -> String {
    format!("{:08x}", zone.unwrap_or(0))
}

/// The spatial partitions and the zone tree of system `zq`. Tier A except
/// the frustum queries (a tangent), whose answers are compared exactly all
/// the same.
#[test]
fn spatial() {
    let universe = query_universe();
    let zq = neuron_core::id::create_id(b"zq");
    let mut partition: Option<SpatialPartition> = None;
    let mut lines = Vec::new();
    for line in input_lines("spatial.list") {
        let (op, rest) = line.split_once(' ').unwrap_or((line.as_str(), ""));
        let words = || -> Vec<f32> { rest.split(" ").map(f32_hex).collect() };
        match op {
            "new" => {
                partition = new_spatial(rest.as_bytes());
                lines.push(
                    Line::new()
                        .add("op", "new")
                        .add("type", rest)
                        .add("made", u8::from(partition.is_some()).to_string())
                        .done(),
                );
            }
            "add" => {
                let (object, values) = rest.split_once(' ').expect("add");
                let w: Vec<f32> = values.split(' ').map(f32_hex).collect();
                if let Some(p) = partition.as_mut() {
                    p.add(SpatialEntry {
                        object: object.parse().expect("object"),
                        position: vec3(&w),
                        radius: w[3],
                    });
                }
            }
            "array" => {
                let entries: Vec<SpatialEntry> = rest
                    .split(' ')
                    .map(|item| {
                        let mut parts = item.split(':');
                        let object = parts.next().expect("object").parse().expect("object");
                        let w: Vec<f32> = parts.map(f32_hex).collect();
                        SpatialEntry {
                            object,
                            position: vec3(&w),
                            radius: w[3],
                        }
                    })
                    .collect();
                if let Some(p) = partition.as_mut() {
                    p.add_array(&entries);
                }
            }
            "remove" => {
                if let Some(p) = partition.as_mut() {
                    p.remove(rest.parse().expect("object"));
                }
            }
            "clear" => {
                if let Some(p) = partition.as_mut() {
                    p.clear();
                }
            }
            "sphere" | "ray" | "frustum" => {
                let w = words();
                let mut result = QueryResult::default();
                let count = match (op, partition.as_ref()) {
                    (_, None) => 0,
                    ("sphere", Some(p)) => p.query_sphere(
                        &Sphere {
                            center: vec3(&w),
                            radius: w[3],
                        },
                        &mut result,
                    ),
                    ("ray", Some(p)) => p.query_ray(
                        &SpatialRay {
                            origin: vec3(&w),
                            direction: vec3(&w[3..]),
                            length: w[6],
                        },
                        &mut result,
                    ),
                    (_, Some(p)) => p.query_frustum(
                        &Frustum {
                            xform: Transform::new(
                                Matrix::from_rows([
                                    [w[0], w[1], w[2]],
                                    [w[3], w[4], w[5]],
                                    [w[6], w[7], w[8]],
                                ]),
                                vec3(&w[9..12]),
                            ),
                            angle_x: w[12],
                            angle_y: w[13],
                            distance: w[14],
                        },
                        &mut result,
                    ),
                };
                lines.push(items_line(op, count, &result));
            }
            _ => {
                let w = words();
                let sphere = Sphere {
                    center: vec3(&w),
                    radius: w[3],
                };
                let p = sphere.center;
                lines.push(
                    Line::new()
                        .add("op", "tree")
                        .add(
                            "sphere_props",
                            format!("{:08x}", universe.sphere_properties(zq, &sphere)),
                        )
                        .add(
                            "nested",
                            format!("{:08x}", universe.nested_properties(zq, &sphere)),
                        )
                        .add("smallest", zone_text(universe.smallest_zone_at(zq, p)))
                        .add(
                            "physical",
                            zone_text(universe.smallest_physical_zone_at(zq, p)),
                        )
                        .add(
                            "with_props",
                            zone_text(universe.smallest_zone_with_properties_at(zq, p, 0x7fc0)),
                        )
                        .done(),
                );
            }
        }
    }
    check_suite("spatial.jsonl", &lines);
}

fn fmt(mad_lib: u32, int: Option<u32>, base: Option<u32>) -> FmtStr {
    let mut out = FmtStr::new(mad_lib);
    if let Some(value) = int {
        out.append_int(value);
    }
    if let Some(id) = base {
        out.append_base(id);
    }
    out
}

fn props_line(record: &str, case: u32, out: &[u8], back: &[u8]) -> String {
    Line::new()
        .add("record", record)
        .add("case", case.to_string())
        .add("out", hex(out))
        .add("back", hex(back))
        .done()
}

/// Marshals a record, reads it back and marshals it again.
fn props_case<T>(
    record: &str,
    case: u32,
    value: &T,
    put: impl Fn(&T, &mut MarshalWriter) -> Result<(), game_sim::protocol::WireError>,
    get: impl Fn(&mut MarshalReader<'_>) -> Result<T, game_sim::protocol::WireError>,
) -> String {
    let mut w = MarshalWriter::new();
    put(value, &mut w).expect("marshal");
    let out = w.into_bytes();
    let back = get(&mut MarshalReader::new(&out)).expect("unmarshal");
    let mut w = MarshalWriter::new();
    put(&back, &mut w).expect("marshal again");
    props_line(record, case, &out, &w.into_bytes())
}

/// The base records' `marshal` and `unmarshal` (Tier A).
#[test]
fn base_props() {
    let mut lines = Vec::new();
    let offers = [
        MissionVendorProperties::default(),
        MissionVendorProperties {
            key: 7,
            base_id: 0x1122_3344,
            target_id: 5,
            system_id: 0xabcd_ef01,
            mission_class: 3,
            title: fmt(1001, Some(42), None),
            description: fmt(1002, None, Some(0x1234)),
            briefing: fmt(0, None, None),
            icon_texture_id: 99,
        },
    ];
    for (case, v) in (0u32..).zip(&offers) {
        lines.push(props_case(
            "mission_vendor",
            case,
            v,
            MissionVendorProperties::marshal,
            MissionVendorProperties::unmarshal,
        ));
    }
    let answers = [
        MissionVendorAcceptance::default(),
        MissionVendorAcceptance {
            key: 2,
            target_id: 3,
            accepted: true,
            reason: fmt(55, Some(1), None),
        },
    ];
    for (case, v) in (0u32..).zip(&answers) {
        lines.push(props_case(
            "acceptance",
            case,
            v,
            MissionVendorAcceptance::marshal,
            MissionVendorAcceptance::unmarshal,
        ));
    }
    let news = [
        NewsBroadcastProperties::default(),
        NewsBroadcastProperties {
            key: 1,
            base_id: 2,
            flag1: true,
            flag2: false,
            category: 4,
            headline: fmt(2001, None, None),
            body: fmt(2002, Some(3), Some(9)),
            logo: fmt(0, None, None),
            logo_texture: Some(b"news_logo".to_vec()),
            logo_texture_id: 0x9988,
        },
    ];
    for (case, v) in (0u32..).zip(&news) {
        lines.push(props_case(
            "news",
            case,
            v,
            NewsBroadcastProperties::marshal,
            NewsBroadcastProperties::unmarshal,
        ));
    }
    let characters = [
        CharacterProperties::default(),
        CharacterProperties {
            key: 1,
            session_id: 2,
            room_id: 3,
            name_ids: 4,
            faction_id: 5,
            name_wide: Some(vec![0x41, 0x62]),
            name_ansi: Some(b"place".to_vec()),
            char_type: 6,
            posture: 7,
            costume: Costume {
                parts: [11, 12, 13, 14],
                accessories: vec![21, 22],
            },
            script: Some(b"script.thn".to_vec()),
            behavior_id: 8,
            voice_id: 9,
        },
    ];
    for (case, v) in (0u32..).zip(&characters) {
        lines.push(props_case(
            "character",
            case,
            v,
            CharacterProperties::marshal,
            CharacterProperties::unmarshal,
        ));
    }
    let scenes = [
        AmbientScriptProperties::default(),
        AmbientScriptProperties {
            key: 4,
            room_id: 5,
            script_id: 6,
            flag: false,
            name: Some(b"amb".to_vec()),
        },
    ];
    for (case, v) in (0u32..).zip(&scenes) {
        lines.push(props_case(
            "ambient",
            case,
            v,
            AmbientScriptProperties::marshal,
            AmbientScriptProperties::unmarshal,
        ));
    }
    check_suite("base_props.jsonl", &lines);
}

/// Work package P2.4's suite: `LookMatrix`, `CEGun::ComputeTimeToTgt`,
/// `CNudgeEngine::SetNudgeVec` and `CSteering::SetAxisThrottle` from rest,
/// over `object_math.list`. Tier A: arithmetic and square roots only.
#[test]
fn object_math() {
    use game_sim::equip::flight::{Nudge, Steering};
    use game_sim::equip::launcher::compute_time_to_target;
    use game_sim::object::csys::look_matrix;

    let v3 = |p: &[&str]| Vector::new(f32_hex(p[0]), f32_hex(p[1]), f32_hex(p[2]));
    let words = |v: &[f32]| v.iter().map(|x| f32_text(*x)).collect::<Vec<_>>().join(",");
    let mut lines = Vec::new();
    for line in input_lines("object_math.list") {
        let parts: Vec<&str> = line.split(' ').collect();
        let args = parts[1..].join(",");
        let out = match parts[0] {
            "look" => words(&look_matrix(v3(&parts[1..4])).to_words()),
            "ttt" => {
                match compute_time_to_target(v3(&parts[1..4]), v3(&parts[4..7]), f32_hex(parts[7]))
                {
                    Some(t) => format!("1,{}", f32_text(t)),
                    None => "0".to_owned(),
                }
            }
            "nudge" => {
                let mut n = Nudge::default();
                n.set_nudge_vec(v3(&parts[1..4]));
                words(&n.current.to_words())
            }
            "axis" => {
                let mut s = Steering::default();
                s.set_axis_throttle(v3(&parts[1..4]));
                words(&s.axis_throttle.to_words())
            }
            other => panic!("object_math.list: unknown kind {other}"),
        };
        lines.push(
            Line::new()
                .add("kind", parts[0].to_owned())
                .add("in", args)
                .add("out", out)
                .done(),
        );
    }
    check_suite("object_math.jsonl", &lines);
}
