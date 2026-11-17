//! Dump lines of the asteroid field files
//! (`CppSource/Oracle/oracle_game_character.cpp` `DumpAsteroidFields`
//! writes the same).
//!
//! The harness walks what the server's `SrvAsteroidSystem::load` walks:
//! every `[Asteroids]` section (with a `file` and a `zone`) of every system
//! file, systems in id order. A field whose zone is missing is not loaded
//! (the server would fault); otherwise the `[Field]` block is read first and
//! the file is then loaded from its start.

use game_sim::data::asteroid_field::{AsteroidField, FieldBlock, FieldCx};
use game_sim::data::config::GameData;
use game_sim::data::{DataSource, Reports, open_ini};
use neuron_core::id::create_id;
use neuron_core::math::Vector;

use super::f32_text as f;
use super::render::{Line, json_text};

fn id(v: u32) -> String {
    format!("{v:08x}")
}

fn b(v: bool) -> String {
    u8::from(v).to_string()
}

fn v3(v: Vector) -> String {
    format!("{},{},{}", f(v.x), f(v.y), f(v.z))
}

/// At most `capacity - 1` bytes (`SrvAsteroid_CopyBounded`).
fn bounded(text: &[u8], capacity: usize) -> Vec<u8> {
    text[..text.len().min(capacity - 1)].to_vec()
}

/// Kind `asteroid_fields`: files `fields` and `loot`.
pub fn asteroid_fields(data: &GameData, source: &dyn DataSource) -> (Vec<String>, Vec<String>) {
    let mut universe = data.universe.clone();
    let mut reports = Reports::default();
    let mut lines = Vec::new();
    let systems: Vec<(u32, Vec<u8>)> = universe
        .systems
        .values()
        .map(|s| {
            (
                s.nickname_id,
                universe.system_file_path(data.strings.get(s.file)),
            )
        })
        .collect();
    for (system, path) in systems {
        let Some(mut reader) = open_ini(source, &path) else {
            continue;
        };
        while reader.read_header() {
            if !reader.is_header(b"Asteroids") {
                continue;
            }
            let (mut file, mut zone_name) = (Vec::new(), Vec::new());
            while reader.read_value() {
                if reader.is_value(b"file") {
                    file = bounded(&reader.value_string(), 0x100);
                } else if reader.is_value(b"zone") {
                    zone_name = bounded(&reader.value_string(), 0x40);
                }
            }
            if file.is_empty() || zone_name.is_empty() {
                continue;
            }
            let mut field = AsteroidField::default();
            field.set_system(system);
            let zone = create_id(&zone_name);
            let zone = universe.zones.contains_key(&zone).then_some(zone);
            field.set_zone(zone);
            let mut loaded = false;
            if zone.is_some()
                && let Some(mut field_reader) = open_ini(source, &file)
            {
                let start = field_reader.state();
                let block = FieldBlock::read(&mut field_reader, field.cube_size);
                field_reader.set_state(&start);
                let mut cx = FieldCx {
                    universe: &mut universe,
                    archetypes: &data.archetypes,
                    reports: &mut reports,
                };
                field.load(&mut field_reader, block.cube_size, &mut cx);
                field.fill_dist = block.fill_dist;
                field.empty_freq = block.empty_cube_frequency as f32;
                loaded = true;
            }
            lines.extend(field_lines(&field, &file, loaded));
        }
    }
    let mut loot = Vec::new();
    for s in universe.systems.values() {
        for zone in s.zones.iter().filter_map(|z| universe.zones.get(z)) {
            if let Some(e) = &zone.lootable {
                loot.push(
                    Line::new()
                        .add("loot_zone", id(zone.nickname))
                        .add(
                            "container",
                            format!(
                                "{},{}",
                                id(e.asteroid_loot_container),
                                id(e.dynamic_loot_container)
                            ),
                        )
                        .add(
                            "commodity",
                            format!(
                                "{},{}",
                                id(e.asteroid_loot_commodity),
                                id(e.dynamic_loot_commodity)
                            ),
                        )
                        .add(
                            "count",
                            format!(
                                "{},{},{},{}",
                                e.asteroid_loot_count_min,
                                e.asteroid_loot_count_max,
                                e.dynamic_loot_count_min,
                                e.dynamic_loot_count_max
                            ),
                        )
                        .add(
                            "difficulty",
                            format!(
                                "{},{}",
                                e.asteroid_loot_difficulty, e.dynamic_loot_difficulty
                            ),
                        )
                        .done(),
                );
            }
        }
    }
    (lines, loot)
}

fn field_lines(field: &AsteroidField, file: &[u8], loaded: bool) -> Vec<String> {
    let mut lines = vec![
        Line::new()
            .add("field", json_text(file))
            .add("system", id(field.system))
            .add("zone", id(field.zone.unwrap_or(0)))
            .add("loaded", b(loaded))
            .add("cube_size", field.cube_size.to_string())
            .add("arch_cube_size", field.arch_cube_size.to_string())
            .add("fill_dist", field.fill_dist.to_string())
            .add("empty_freq", f(field.empty_freq))
            .done(),
    ];
    let rotations: Vec<String> = field
        .rotations
        .angles
        .iter()
        .flat_map(|axis| axis.iter().map(|a| f(*a)))
        .collect();
    lines.push(Line::new().add("rotations", rotations.join(",")).done());
    for a in &field.arch_list {
        let orient: Vec<String> = a.orient.m.iter().flatten().map(|x| f(*x)).collect();
        lines.push(
            Line::new()
                .add("arch", id(a.arch))
                .add("extent", v3(a.extent))
                .add("orient", orient.join(","))
                .add("mine", b(a.mine))
                .add("instant_kill", b(a.instant_kill))
                .done(),
        );
    }
    if let Some(mgr) = &field.exclusion {
        for z in &mgr.zones {
            lines.push(
                Line::new()
                    .add("exclusion", id(z.zone.as_ref().map_or(0, |z| z.nickname)))
                    .add("billboards", b(z.exclude_billboards))
                    .add("dynamic", b(z.exclude_dynamic_asteroids))
                    .add("empty", f(z.empty_cube_frequency))
                    .add("count", f(z.billboard_count))
                    .add("dist", f(z.max_dist_sq))
                    .done(),
            );
        }
    }
    let loot: Vec<String> = field.loot_zones.iter().map(|z| id(*z)).collect();
    lines.push(Line::new().add("loot_zones", loot.join(";")).done());
    lines
}
