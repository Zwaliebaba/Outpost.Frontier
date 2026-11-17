//! Oracle vectors for work package P2.3: the exact pieces of
//! `neuron_core::physics` against Common.dll (`CppSource/Oracle/
//! oracle_physics.cpp`): `Collision::CalcCollisionDamage` (both overloads),
//! `GetImpactDamageFromType` and `Load`; `CDeadReckonedVector`; and
//! `CRemotePhysicsSimulation`.
//!
//! Inputs are in `tests/fixtures/oracle/physics/`, written by
//! `make_physics_fixtures.py`. Every number is a bit pattern in hex (`f32`
//! in eight digits, `f64` in sixteen, vectors concatenated). The ports are
//! exact on the 53-bit model (decision T11), so the comparison is byte for
//! byte; both sides write every NaN as one pattern (NaN payloads are not
//! part of the port).
//!
//! Two harness choices mirror the port where the reference is undefined
//! (divergence D94): `CRemotePhysicsSimulation`'s orientation starts at the
//! identity, and the samples of a new `CDeadReckonedVector` are zero (as
//! the C++ constructor already makes them).

mod support;

use support::{check_suite, input_lines};

use neuron_core::ini::IniReader;
use neuron_core::math::{Quaternion, Vector};
use neuron_core::physics::{
    CollisionDamage, DeadReckonedVector, RemotePhysicsSimulation, calc_collision_damage_scaled,
};

/// `DecodeType`'s table (`common_archetype.cpp`; `game-sim` owns the port,
/// `data::archetype::object_type`): the first name `stricmp` matches.
const OBJECT_TYPES: [(&str, u32); 29] = [
    ("NONE", 0),
    ("MOON", 1),
    ("PLANET", 2),
    ("SUN", 4),
    ("ASTEROID", 0x2000_0000),
    ("BLACKHOLE", 8),
    ("SATELLITE", 0x10),
    ("DOCKING_RING", 0x20),
    ("JUMP_GATE", 0x40),
    ("JUMP_HOLE", 0x800),
    ("WEAPONS_PLATFORM", 0x1000),
    ("DESTROYABLE_DEPOT", 0x2000),
    ("NON_TARGETABLE", 0x4000),
    ("MISSION_SATELLITE", 0x8000),
    ("AIRLOCK_GATE", 0x400),
    ("TRADELANE_RING", 0x80),
    ("STATION", 0x100),
    ("WAYPOINT", 0x200),
    ("FIGHTER", 0x1_0000),
    ("FREIGHTER", 0x2_0000),
    ("GUNBOAT", 0x4_0000),
    ("CRUISER", 0x8_0000),
    ("TRANSPORT", 0x10_0000),
    ("CAPITAL", 0x20_0000),
    ("MINING", 0x40_0000),
    ("GUIDED", 0x100_0000),
    ("MINE", 0x400_0000),
    ("LOOT", 0x1000_0000),
    ("BULLET", 0x200_0000),
];

fn decode_type(name: &[u8]) -> u32 {
    OBJECT_TYPES
        .iter()
        .find(|(n, _)| n.as_bytes().eq_ignore_ascii_case(name))
        .map_or(0, |(_, bits)| *bits)
}

fn parse_u32(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn parse_f32(text: &str) -> f32 {
    f32::from_bits(parse_u32(text))
}

fn parse_f64(text: &str) -> f64 {
    f64::from_bits(u64::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

fn parse_vec(words: &[&str]) -> Vector {
    Vector::new(
        parse_f32(words[0]),
        parse_f32(words[1]),
        parse_f32(words[2]),
    )
}

fn parse_quat(words: &[&str]) -> Quaternion {
    Quaternion::new(
        parse_f32(words[0]),
        parse_f32(words[1]),
        parse_f32(words[2]),
        parse_f32(words[3]),
    )
}

/// An `f32` in eight hex digits, every NaN as `7fc00000`.
fn h32(value: f32) -> String {
    let bits = if value.is_nan() {
        0x7fc0_0000
    } else {
        value.to_bits()
    };
    format!("{bits:08x}")
}

/// An `f64` in sixteen hex digits, every NaN as `7ff8000000000000`.
fn h64(value: f64) -> String {
    let bits = if value.is_nan() {
        0x7ff8_0000_0000_0000
    } else {
        value.to_bits()
    };
    format!("{bits:016x}")
}

fn hv(v: Vector) -> String {
    format!("{}{}{}", h32(v.x), h32(v.y), h32(v.z))
}

fn hq(q: Quaternion) -> String {
    format!("{}{}{}{}", h32(q.w), h32(q.x), h32(q.y), h32(q.z))
}

#[test]
fn physics_damage() {
    let mut table = CollisionDamage::default();
    let mut lines = Vec::new();
    for line in input_lines("physics/damage.txt") {
        let words: Vec<&str> = line.split(' ').collect();
        let out = match words[0] {
            "load" => {
                table.free();
                let text = support::read_fixture(words[1]);
                let mut reader = IniReader::new();
                let mut ok = String::new();
                if reader.open_memory(&text) {
                    while reader.read_header() {
                        ok.push(if table.load(&mut reader, decode_type) {
                            '1'
                        } else {
                            '0'
                        });
                    }
                }
                format!("\"ok\":\"{ok}\"")
            }
            "type" => format!(
                "\"damage\":\"{}\"",
                h32(table.impact_damage_from_type(parse_u32(words[1])))
            ),
            "calc" => format!(
                "\"damage\":\"{}\"",
                h32(table.calc_collision_damage(
                    parse_u32(words[1]),
                    parse_vec(&words[2..5]),
                    parse_vec(&words[5..8]),
                ))
            ),
            "scaled" => format!(
                "\"damage\":\"{}\"",
                h32(calc_collision_damage_scaled(
                    parse_f32(words[1]),
                    parse_f32(words[2]),
                    parse_f32(words[3]),
                    parse_f32(words[4]),
                ))
            ),
            other => panic!("physics/damage.txt: unknown op {other}"),
        };
        lines.push(format!("{{\"in\":\"{line}\",{out}}}"));
    }
    check_suite("physics_damage.jsonl", &lines);
}

fn drv_line(line: &str, d: &DeadReckonedVector, probe_t: f64, probe_p: Vector) -> String {
    let samples: String = d.samples.iter().map(|&v| hv(v)).collect();
    let stamps: String = d.timestamps.iter().map(|&t| h64(t)).collect();
    format!(
        "{{\"in\":\"{line}\",\"samples\":\"{samples}\",\"stamps\":\"{stamps}\",\"base\":\"{}\",\"blend_base\":\"{}\",\"projected\":\"{}\",\"tracked\":\"{}\",\"velocity\":\"{}\",\"velocity2\":\"{}\",\"converged\":\"{}\",\"accel\":\"{}\",\"accel2\":\"{}\",\"count\":\"{:08x}\",\"window\":\"{}\",\"elapsed\":\"{}\",\"max_corrective\":\"{}\",\"project_velocity\":\"{}\",\"error\":\"{}\"}}",
        hv(d.base_position),
        hv(d.blend_base_position),
        hv(d.projected_position),
        hv(d.tracked_position),
        hv(d.velocity),
        hv(d.velocity2),
        hv(d.converged_velocity),
        hv(d.acceleration),
        hv(d.acceleration2),
        d.sample_count,
        h64(d.converge_time()),
        h64(d.elapsed_time),
        h64(d.max_corrective_time),
        hv(d.project_velocity(probe_t)),
        h64(d.calc_error(probe_p)),
    )
}

#[test]
fn physics_drv() {
    let mut lines = Vec::new();
    let mut d = DeadReckonedVector::default();
    let mut probe_t = 0.0;
    let mut probe_p = Vector::ZERO;
    for line in input_lines("physics/drv.txt") {
        let words: Vec<&str> = line.split(' ').collect();
        match words[0] {
            "seq" => {
                d = DeadReckonedVector::default();
                probe_t = parse_f64(words[1]);
                probe_p = parse_vec(&words[2..5]);
            }
            "add" => d.add_sample(parse_vec(&words[1..4]), parse_f64(words[4])),
            "doit" => d.doit(parse_f64(words[1])),
            "maxc" => d.set_max_corrective_converge_time(parse_f64(words[1])),
            "reset" => d.reset(),
            "normp" => d.normalize_projected_position(),
            "normt" => d.normalize_tracked_position(),
            other => panic!("physics/drv.txt: unknown op {other}"),
        }
        lines.push(drv_line(&line, &d, probe_t, probe_p));
    }
    check_suite("physics_drv.jsonl", &lines);
}

#[test]
fn physics_remote() {
    let mut lines = Vec::new();
    let mut r = RemotePhysicsSimulation::default();
    for line in input_lines("physics/remote.txt") {
        let words: Vec<&str> = line.split(' ').collect();
        let mut sync = "-";
        match words[0] {
            "seq" => r = RemotePhysicsSimulation::default(),
            "update" => r.remote_update(
                parse_vec(&words[1..4]),
                parse_quat(&words[4..8]),
                parse_f64(words[8]),
            ),
            "doit" => r.doit(parse_f64(words[1])),
            "sync" => {
                let force = r.check_for_sync(
                    parse_vec(&words[1..4]),
                    parse_vec(&words[4..7]),
                    parse_quat(&words[7..11]),
                );
                sync = if force { "1" } else { "0" };
            }
            "reset" => r.reset(),
            other => panic!("physics/remote.txt: unknown op {other}"),
        }
        lines.push(format!(
            "{{\"in\":\"{line}\",\"velocity\":\"{}\",\"blend_from\":\"{}\",\"blend_to\":\"{}\",\"orientation\":\"{}\",\"projected\":\"{}\",\"count\":\"{:08x}\",\"time\":\"{}\",\"window\":\"{}\",\"backoff\":\"{}\",\"pos_tracked\":\"{}\",\"x_tracked\":\"{}\",\"y_tracked\":\"{}\",\"sync\":\"{sync}\"}}",
            hv(r.velocity),
            hv(r.blend_from_velocity),
            hv(r.blend_to_velocity),
            hq(r.orientation),
            hv(r.projected_position),
            r.sample_count(),
            h64(r.simulation_time()),
            h64(r.converge_window),
            h64(r.sync_backoff),
            hv(r.position_drv.tracked_position),
            hv(r.orient_axis_x_drv.tracked_position),
            hv(r.orient_axis_y_drv.tracked_position),
        ));
    }
    check_suite("physics_remote.jsonl", &lines);
}
