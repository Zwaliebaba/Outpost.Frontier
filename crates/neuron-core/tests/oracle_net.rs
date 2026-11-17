//! Oracle vectors for work package P1.9: the wire primitives and the
//! quantisers of `neuron_core::net::wire` against the C++ codecs
//! (`CppSource/Oracle/oracle_net.cpp`, which calls `RemoteCommon/` as
//! `RemoteClient.dll` compiles it).
//!
//! Inputs are in `tests/fixtures/oracle/net/`, written by
//! `make_net_fixtures.py`. The framing, the handshake and the transports
//! are new and have no C++ counterpart; their tests are in the modules.

mod support;

use support::{bits32, check_suite, hex, input_lines, unhex};

use neuron_core::net::wire::quantise::{
    UNKNOWN_ARCHETYPE_MAX_HIT_POINTS, expand_fire_medium, expand_fire_small, expand_fraction,
    expand_hull, expand_tenths, expand_unit, fire_tier, get_fire_component, put_fire_component,
    quantise_fire_medium, quantise_fire_small, quantise_fraction, quantise_hull, quantise_tenths,
    quantise_unit,
};
use neuron_core::net::wire::{MessageHeader, WireError, WireReader, WireWriter};

/// `FLPACKET_SERVER_SETHULLSTATUS`, a scalar `f32` packet.
const SET_HULL_STATUS: u32 = 0x2_0049;
/// `FLPACKET_SERVER_CHARSELECTVERIFIED`: a `u32`, then an x87 `f64`.
const CHAR_SELECT_VERIFIED: u32 = 0x2_0008;
const REP_INSTANCE: u32 = 0x0403_0201;

fn parse_u32(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn header_for(packet_id: u32) -> MessageHeader {
    MessageHeader::from_word(MessageHeader::word_for_packet_id(packet_id))
}

fn code(error: &WireError) -> String {
    error
        .legacy_code()
        .map_or_else(|| format!("{error:?}"), |c| format!("{c:02x}"))
}

#[test]
fn net_quantise() {
    let lines: Vec<String> = input_lines("net/quantise.hex")
        .iter()
        .map(|line| {
            let value = f32::from_bits(parse_u32(line));
            let tier_xy = fire_tier(value, true);
            let tier_z = fire_tier(value, false);
            let mut fire = WireWriter::with_limit(4);
            put_fire_component(&mut fire, value, tier_xy).unwrap();
            let fire = fire.into_bytes();
            let back = get_fire_component(&mut WireReader::new(&fire), tier_xy).unwrap();
            let mut hull_status = WireWriter::with_limit(16);
            hull_status.put_header(header_for(SET_HULL_STATUS)).unwrap();
            hull_status.put_f32_x87(value).unwrap();
            let unit = quantise_unit(value);
            format!(
                "{{\"in\":\"{line}\",\"unit\":\"{unit:02x}\",\"update\":\"{unit:02x}\",\"fraction\":\"{:02x}\",\"tenths\":\"{:02x}\",\"tier\":\"{}{}\",\"small\":\"{:02x}\",\"medium\":\"{:04x}\",\"fire\":\"{}\",\"fire_back\":\"{:08x}\",\"hull_status\":\"{}\"}}",
                quantise_fraction(value),
                quantise_tenths(value),
                tier_xy.bits(),
                tier_z.bits(),
                quantise_fire_small(value),
                quantise_fire_medium(value),
                hex(&fire),
                back.to_bits(),
                hex(hull_status.as_bytes()),
            )
        })
        .collect();
    check_suite("net_quantise.jsonl", &lines);
}

#[test]
fn net_expand() {
    let lines: Vec<String> = input_lines("net/expand.hex")
        .iter()
        .map(|line| {
            let word = u16::try_from(parse_u32(line)).unwrap();
            let [byte, _] = word.to_le_bytes();
            let unit = expand_unit(byte).to_bits();
            format!(
                "{{\"in\":\"{line}\",\"unit\":\"{unit:08x}\",\"update\":\"{unit:08x}\",\"fraction\":\"{:08x}\",\"tenths\":\"{:08x}\",\"small\":\"{:08x}\",\"medium\":\"{:08x}\",\"hull\":\"{:08x}\"}}",
                expand_fraction(byte).to_bits(),
                expand_tenths(byte).to_bits(),
                expand_fire_small(byte).to_bits(),
                expand_fire_medium(word).to_bits(),
                bits32(expand_hull(byte, UNKNOWN_ARCHETYPE_MAX_HIT_POINTS)),
            )
        })
        .collect();
    check_suite("net_expand.jsonl", &lines);
}

#[test]
fn net_hull() {
    let lines: Vec<String> = input_lines("net/hull.hex")
        .iter()
        .map(|line| {
            let hit_points = i32::from_ne_bytes(parse_u32(line).to_ne_bytes());
            format!(
                "{{\"in\":\"{line}\",\"hull\":\"{:02x}\"}}",
                quantise_hull(hit_points, UNKNOWN_ARCHETYPE_MAX_HIT_POINTS)
            )
        })
        .collect();
    check_suite("net_hull.jsonl", &lines);
}

#[test]
fn net_doubles() {
    let lines: Vec<String> = input_lines("net/doubles.hex")
        .iter()
        .map(|line| {
            let bits = u64::from_str_radix(line, 16).unwrap();
            let mut packet = WireWriter::with_limit(32);
            packet.put_header(header_for(CHAR_SELECT_VERIFIED)).unwrap();
            packet.put_u32(REP_INSTANCE).unwrap();
            packet.put_f64_x87(f64::from_bits(bits)).unwrap();
            format!(
                "{{\"in\":\"{line}\",\"packet\":\"{}\"}}",
                hex(packet.as_bytes())
            )
        })
        .collect();
    check_suite("net_doubles.jsonl", &lines);
}

#[test]
fn net_header() {
    let lines: Vec<String> = input_lines("net/header.hex")
        .iter()
        .map(|line| {
            let word = MessageHeader::word_for_packet_id(parse_u32(line));
            format!(
                "{{\"in\":\"{line}\",\"header\":\"{}\"}}",
                hex(&word.to_le_bytes())
            )
        })
        .collect();
    check_suite("net_header.jsonl", &lines);
}

fn units(text: &str) -> Vec<u16> {
    (0..text.len())
        .step_by(4)
        .map(|i| u16::from_str_radix(&text[i..i + 4], 16).unwrap())
        .collect()
}

fn write_op(writer: &mut WireWriter, name: &str, arg: &str) -> Result<(), WireError> {
    match name {
        "u8" => writer.put_u8(u8::try_from(parse_u32(arg)).unwrap()),
        "u16" => writer.put_u16(u16::try_from(parse_u32(arg)).unwrap()),
        "u32" => writer.put_u32(parse_u32(arg)),
        "f32" => writer.put_f32(f32::from_bits(parse_u32(arg))),
        "f32x" => writer.put_f32_x87(f32::from_bits(parse_u32(arg))),
        "vec" => writer.put_vector([0, 8, 16].map(|i| f32::from_bits(parse_u32(&arg[i..i + 8])))),
        "bytes" => writer.put_bytes(&unhex(arg)),
        "b16" => writer.put_bytes_u16(&unhex(arg)),
        "b32" => writer.put_bytes_u32(&unhex(arg)),
        "wide16" | "text16" => writer.put_wide_u16(&units(arg)),
        other => panic!("unknown write op {other}"),
    }
}

#[test]
fn net_write() {
    let lines: Vec<String> = input_lines("net/write.txt")
        .iter()
        .map(|line| {
            let mut words = line.split(' ');
            let limit: usize = words.next().unwrap().parse().unwrap();
            let mut writer = WireWriter::with_limit(limit);
            let mut result = "00".to_owned();
            let mut failed = -1;
            let mut out = None;
            for (index, op) in (0..).zip(words) {
                let (name, arg) = op.split_once(':').unwrap();
                let start = writer.len();
                if let Err(error) = write_op(&mut writer, name, arg) {
                    result = code(&error);
                    failed = index;
                    out = Some(hex(&writer.as_bytes()[..start]));
                    break;
                }
            }
            let out = out.unwrap_or_else(|| hex(writer.as_bytes()));
            format!("{{\"in\":\"{line}\",\"result\":\"{result}\",\"failed\":\"{failed}\",\"out\":\"{out}\"}}")
        })
        .collect();
    check_suite("net_write.jsonl", &lines);
}

fn wide_hex(units: &[u16]) -> String {
    units.iter().map(|u| format!("{u:04x}")).collect()
}

fn read_op(reader: &mut WireReader<'_>, op: &str) -> Result<String, WireError> {
    let (name, arg) = op.split_once(':').unwrap_or((op, ""));
    Ok(match name {
        "u8" => format!("{:02x}", reader.get_u8()?),
        "u16" => format!("{:04x}", reader.get_u16()?),
        "u32" => format!("{:08x}", reader.get_u32()?),
        "f32" => format!("{:08x}", reader.get_f32()?.to_bits()),
        "f64" => format!("{:016x}", reader.get_f64()?.to_bits()),
        "vec" => reader
            .get_vector()?
            .iter()
            .map(|c| format!("{:08x}", c.to_bits()))
            .collect(),
        "bytes" => hex(reader.get_bytes(arg.parse().unwrap())?),
        "wide16" | "text16" => wide_hex(&reader.get_wide_u16()?),
        "b32" => hex(reader.get_bytes_u32()?),
        other => panic!("unknown read op {other}"),
    })
}

#[test]
fn net_read() {
    let lines: Vec<String> = input_lines("net/read.txt")
        .iter()
        .map(|line| {
            let (data, ops) = line.split_once(' ').unwrap();
            let data = unhex(data);
            let mut reader = WireReader::new(&data);
            let mut values = Vec::new();
            let mut result = "00".to_owned();
            let mut failed = -1;
            for (index, op) in (0..).zip(ops.split(' ')) {
                match read_op(&mut reader, op) {
                    Ok(value) => values.push(value),
                    Err(error) => {
                        result = code(&error);
                        failed = index;
                        break;
                    }
                }
            }
            let position = if failed < 0 {
                reader.position().to_string()
            } else {
                "-".to_owned()
            };
            format!(
                "{{\"in\":\"{line}\",\"values\":\"{}\",\"result\":\"{result}\",\"failed\":\"{failed}\",\"pos\":\"{position}\"}}",
                values.join(".")
            )
        })
        .collect();
    check_suite("net_read.jsonl", &lines);
}
