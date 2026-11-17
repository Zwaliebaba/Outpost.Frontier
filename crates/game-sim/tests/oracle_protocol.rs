//! Oracle suites of work package P2.2: the message codecs and the
//! `Common` encodings, against `CppSource/Oracle/oracle_protocol*.cpp`.
//!
//! Every packet line is `{"in":"<name> <mode> <case>","out":"<hex>"}`: the
//! C++ encodes the synthetic packet of the case ([`support::fill`]) with
//! its real `Encode`, the bytes with the header. Here the same values are
//! encoded and must give the same bytes; the bytes are decoded, must give
//! the values back, and must re-encode to themselves.
//!
//! `OUTPOST_BLESS_VECTORS=1 cargo test --test oracle_protocol` rewrites the
//! suites (only to add a suite; see `AGENTS.md`).

mod support;

use game_sim::protocol::costume::Costume;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::marshal::{EquipDesc, GoodDesc, MarshalReader, MarshalWriter};
use game_sim::protocol::rdl::{RdlRecord, extract_text, read_records, write_records};
use game_sim::protocol::to_server::{Login, lower_ascii_units};
use game_sim::protocol::{
    ClientMessage, CommonMessage, ServerMessage, WireContext, WireError, WireWriter,
};
use support::fill::{self, Entry, Mode};
use support::{Gen, check_suite, hex, wide_hex};

fn packet_suite<M: PartialEq + std::fmt::Debug>(
    entries: &[Entry],
    channel: u8,
    make: impl Fn(u32, u32, Mode, &mut Gen) -> M,
    encode: impl Fn(&M, &WireContext) -> Result<Vec<u8>, WireError>,
    decode: impl Fn(&WireContext, &[u8]) -> Result<M, WireError>,
    normalize: impl Fn(&M) -> M,
) -> Vec<String> {
    let mut lines = Vec::new();
    for entry in entries {
        for &mode in entry.modes {
            let ctx = mode.context(channel);
            for case in 0..entry.cases {
                let mut g = Gen::for_case(entry.id, case);
                let message = make(entry.id, case, mode, &mut g);
                let label = format!("{} {} {case}", entry.name, mode.name());
                let bytes = encode(&message, &ctx).unwrap_or_else(|e| panic!("{label}: {e}"));
                let back = decode(&ctx, &bytes).unwrap_or_else(|e| panic!("{label}: {e}"));
                assert_eq!(back, normalize(&message), "{label}: decode");
                assert_eq!(
                    encode(&back, &ctx).as_deref(),
                    Ok(&bytes[..]),
                    "{label}: re-encode"
                );
                lines.push(format!(
                    "{{\"in\":\"{label}\",\"out\":\"{}\"}}",
                    hex(&bytes)
                ));
            }
        }
    }
    lines
}

#[test]
fn protocol_common() {
    let lines = packet_suite(
        fill::COMMON,
        1,
        fill::common,
        CommonMessage::to_wire,
        CommonMessage::from_wire,
        Clone::clone,
    );
    check_suite("protocol_common.jsonl", &lines);
}

/// The account name travels lower-cased.
fn normalize_client(message: &ClientMessage) -> ClientMessage {
    match message {
        ClientMessage::Login(login) => ClientMessage::Login(Login {
            account_name: lower_ascii_units(&login.account_name),
            ..login.clone()
        }),
        other => other.clone(),
    }
}

#[test]
fn protocol_client() {
    let lines = packet_suite(
        fill::CLIENT,
        3,
        |id, case, _, g| fill::client(id, case, g),
        ClientMessage::to_wire,
        ClientMessage::from_wire,
        normalize_client,
    );
    check_suite("protocol_client.jsonl", &lines);
}

#[test]
fn protocol_server() {
    let lines = packet_suite(
        fill::SERVER,
        2,
        fill::server,
        ServerMessage::to_wire,
        ServerMessage::from_wire,
        Clone::clone,
    );
    check_suite("protocol_server.jsonl", &lines);
}

/// The format string of case `case` (`FmtStrCase` in the harness).
pub fn fmt_case(case: u32, g: &mut Gen) -> FmtStr {
    let mut fmt = FmtStr::new(g.u32());
    match case {
        0 => {}
        1 => {
            fmt.append_system(g.u32());
            fmt.append_int(g.u32());
            fmt.append_string(g.u32());
            fmt.append_base(g.u32());
            fmt.append_good(g.u32());
            fmt.append_rep_instance(g.i32());
            fmt.append_rep_group(g.u32());
            fmt.append_zone_id(g.u32());
            fmt.append_spaceobj_id(g.u32());
            fmt.append_installation(g.i32());
            fmt.append_loot(g.i32());
            fmt.append_system(g.u32());
            let mut marker = [0; 5];
            for word in &mut marker {
                *word = g.u32();
            }
            fmt.append_nav_marker(marker);
        }
        _ => {
            let mut inner = FmtStr::new(g.u32());
            inner.append_int(g.u32());
            inner.append_system(g.u32());
            let mut middle = FmtStr::new(g.u32());
            middle.append_fmt_str(&inner);
            middle.append_loot(g.i32());
            fmt.append_fmt_str(&inner);
            fmt.append_int(g.u32());
            fmt.append_fmt_str(&middle);
        }
    }
    fmt
}

#[test]
fn protocol_fmtstr() {
    let mut lines = Vec::new();
    for case in 0..3 {
        let mut g = Gen::for_case(0xf3, case);
        let fmt = fmt_case(case, &mut g);
        let flat = fmt.flatten().unwrap();
        let (back, used) = FmtStr::unflatten(&flat).unwrap();
        assert_eq!(back, fmt);
        let again = back.flatten().unwrap();
        lines.push(format!(
            "{{\"in\":\"fmtstr {case}\",\"flat\":\"{}\",\"size\":\"{}\",\"used\":\"{used}\",\"back\":\"{}\"}}",
            hex(&flat),
            fmt.flattened_size(),
            hex(&again)
        ));
    }
    check_suite("protocol_fmtstr.jsonl", &lines);
}

fn rdl_header(out: &mut Vec<u8>, kind: i32, length: u32) {
    out.extend_from_slice(&kind.to_le_bytes());
    out.extend_from_slice(&length.to_le_bytes());
}

fn rdl_text(out: &mut Vec<u8>, g: &mut Gen, count: usize) {
    rdl_header(out, 2, u32::try_from(count * 2 + 2).unwrap());
    for _ in 0..count {
        out.extend_from_slice(&g.text_unit().to_le_bytes());
    }
    out.extend_from_slice(&[0, 0]);
}

/// The record bytes of case `case` (`RdlCase` in the harness).
fn rdl_case(case: u32, g: &mut Gen) -> Vec<u8> {
    let mut out = Vec::new();
    match case {
        0 => {}
        1 => {
            rdl_header(&mut out, 1, 8);
            out.extend_from_slice(&g.u32().to_le_bytes());
            out.extend_from_slice(&g.u32().to_le_bytes());
            rdl_text(&mut out, g, 3);
            rdl_header(&mut out, 4, 9);
            out.push(g.u8() & 3);
            out.extend_from_slice(&g.f32().to_bits().to_le_bytes());
            out.extend_from_slice(&g.f32().to_bits().to_le_bytes());
            rdl_header(&mut out, 5, 0);
            rdl_header(&mut out, 6, 2);
            out.extend_from_slice(&g.u16().to_le_bytes());
            rdl_text(&mut out, g, 0);
        }
        2 => {
            rdl_text(&mut out, g, 5);
            rdl_header(&mut out, 3, 4);
            for _ in 0..4 {
                out.push(g.u8());
            }
            rdl_header(&mut out, 9, 1);
            out.push(g.u8());
            rdl_text(&mut out, g, 2);
        }
        _ => {
            rdl_header(&mut out, 5, 2);
            out.extend_from_slice(&[0, 0]);
            rdl_header(&mut out, 2, 0);
            rdl_header(&mut out, 6, 2);
            out.extend_from_slice(&g.u16().to_le_bytes());
            rdl_text(&mut out, g, 7);
        }
    }
    out
}

#[test]
fn protocol_rdl() {
    let mut lines = Vec::new();
    for case in 0..4 {
        let mut g = Gen::for_case(0xd1, case);
        let input = rdl_case(case, &mut g);
        let (records, read_ok): (Vec<RdlRecord>, bool) = read_records(&input);
        let (full, full_ok) = write_records(&records, 0x1000);
        let (part, part_ok) = write_records(&records, 0x18);
        let (short, short_ok) = extract_text(&input, 4);
        let (long, long_ok) = extract_text(&input, 100);
        lines.push(format!(
            "{{\"in\":\"{}\",\"read\":\"{}\",\"write\":\"{}\",\"write_ok\":\"{}\",\"part\":\"{}\",\"part_ok\":\"{}\",\"text4\":\"{}\",\"text4_ok\":\"{}\",\"text\":\"{}\",\"text_ok\":\"{}\"}}",
            hex(&input),
            u8::from(read_ok),
            hex(&full),
            u8::from(full_ok),
            hex(&part),
            u8::from(part_ok),
            wide_hex(&short),
            u8::from(short_ok),
            wide_hex(&long),
            u8::from(long_ok),
        ));
    }
    check_suite("protocol_rdl.jsonl", &lines);
}

fn opt_hex(bytes: Option<&[u8]>) -> String {
    bytes.map_or_else(|| "null".to_owned(), hex)
}

fn opt_wide(units: Option<&[u16]>) -> String {
    units.map_or_else(|| "null".to_owned(), wide_hex)
}

/// The goods and equipment of marshal case 2 (`MarshalLists` in the
/// harness).
fn marshal_lists(g: &mut Gen) -> (Costume, Vec<GoodDesc>, Vec<EquipDesc>) {
    let mut parts = [0; 4];
    for part in &mut parts {
        *part = g.u32();
    }
    let costume = Costume {
        parts,
        accessories: g.dwords(3),
    };
    let goods = (0..2)
        .map(|_| {
            let good_id = g.u32();
            let count = g.i32();
            let price = g.f32();
            GoodDesc {
                good_id,
                count,
                price,
                field_0c: g.f32(),
            }
        })
        .collect();
    let equip = ["HpWeapon01", "HpTurret02"]
        .iter()
        .map(|name| {
            let arch_id = g.u32();
            let count = g.i32();
            let health = g.f32();
            let id = g.u16();
            let equipped = g.u8() & 1 != 0;
            let mission = g.u8() & 1 != 0;
            EquipDesc {
                arch_id,
                hardpoint: Some(name.as_bytes().to_vec()),
                count,
                health,
                id,
                equipped,
                mission,
            }
        })
        .collect();
    (costume, goods, equip)
}

#[test]
fn protocol_marshal() {
    let mut lines = Vec::new();

    // Case 0: scalars.
    let mut g = Gen::for_case(0x3a, 0);
    let mut w = MarshalWriter::new();
    let (b, s, d, i, f, id) = (g.u8(), g.u16(), g.u32(), g.i32(), g.f32(), g.u32());
    w.put_u8(b).unwrap();
    w.put_u16(s).unwrap();
    w.put_u32(d).unwrap();
    w.put_int(i).unwrap();
    w.put_float(f).unwrap();
    w.put_bool(true).unwrap();
    w.put_bool(false).unwrap();
    w.put_u32(id).unwrap();
    let out = w.into_bytes();
    let mut r = MarshalReader::new(&out);
    let values = format!(
        "{:02x}.{:04x}.{:08x}.{:08x}.{:08x}.{}.{}.{:08x}",
        r.get_u8().unwrap(),
        r.get_u16().unwrap(),
        r.get_u32().unwrap(),
        r.get_int().unwrap(),
        r.get_float().unwrap().to_bits(),
        u8::from(r.get_bool().unwrap()),
        u8::from(r.get_bool().unwrap()),
        r.get_u32().unwrap(),
    );
    lines.push(format!(
        "{{\"in\":\"marshal 0\",\"out\":\"{}\",\"values\":\"{values}\"}}",
        hex(&out)
    ));

    // Case 1: strings.
    let mut g = Gen::for_case(0x3a, 1);
    let text: Vec<u8> = (0..5).map(|_| g.text_byte()).collect();
    let mut w = MarshalWriter::new();
    w.put_string(None).unwrap();
    w.put_string(Some(b"")).unwrap();
    w.put_string(Some(b"abc")).unwrap();
    w.put_wstring(None).unwrap();
    w.put_wstring(Some(&[0x48, 0x69, 0x21])).unwrap();
    w.put_string(Some(&text)).unwrap();
    let out = w.into_bytes();
    let mut r = MarshalReader::new(&out);
    let values = [
        opt_hex(r.get_string().unwrap().as_deref()),
        opt_hex(r.get_string().unwrap().as_deref()),
        hex(&r.get_string_to_buffer(3).unwrap()),
        opt_wide(r.get_wstring().unwrap().as_deref()),
        opt_wide(r.get_wstring().unwrap().as_deref()),
        opt_hex(r.get_string().unwrap().as_deref()),
    ]
    .join(".");
    lines.push(format!(
        "{{\"in\":\"marshal 1\",\"out\":\"{}\",\"values\":\"{values}\"}}",
        hex(&out)
    ));

    // Case 2: costume and lists, then what reading them back gives.
    let mut g = Gen::for_case(0x3a, 2);
    let (costume, goods, equip) = marshal_lists(&mut g);
    let mut w = MarshalWriter::new();
    w.put_costume(&costume).unwrap();
    w.put_good_desc_list(&goods).unwrap();
    w.put_equip_desc_list(&equip).unwrap();
    let out = w.into_bytes();
    let mut r = MarshalReader::new(&out);
    let mut back = MarshalWriter::new();
    back.put_costume(&r.get_costume().unwrap()).unwrap();
    back.put_good_desc_list(&r.get_good_desc_list().unwrap())
        .unwrap();
    back.put_equip_desc_list(&r.get_equip_desc_list().unwrap())
        .unwrap();
    lines.push(format!(
        "{{\"in\":\"marshal 2\",\"out\":\"{}\",\"back\":\"{}\"}}",
        hex(&out),
        hex(back.as_bytes())
    ));

    // Case 3: a format string.
    let mut g = Gen::for_case(0xf3, 2);
    let fmt = fmt_case(2, &mut g);
    let mut w = MarshalWriter::new();
    w.put_fmt_str(&fmt).unwrap();
    w.put_u8(0x5a).unwrap();
    let out = w.into_bytes();
    let mut r = MarshalReader::new(&out);
    let mut back = MarshalWriter::new();
    back.put_fmt_str(&r.get_fmt_str().unwrap()).unwrap();
    back.put_u8(r.get_u8().unwrap()).unwrap();
    lines.push(format!(
        "{{\"in\":\"marshal 3\",\"out\":\"{}\",\"back\":\"{}\"}}",
        hex(&out),
        hex(back.as_bytes())
    ));

    // Case 4: reads of a crafted buffer: an over-long string reads as null
    // and only its length is consumed; a bool byte of 2 is false.
    let input = [
        0x00, 0x40, 0x00, 0x00, 0x41, 0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0x07,
    ];
    let mut r = MarshalReader::new(&input);
    let values = [
        opt_hex(r.get_string().unwrap().as_deref()),
        format!("{:02x}", r.get_u8().unwrap()),
        u8::from(r.get_bool().unwrap()).to_string(),
        u8::from(r.get_bool().unwrap()).to_string(),
        opt_wide(r.get_wstring().unwrap().as_deref()),
        format!("{:02x}", r.get_u8().unwrap()),
    ]
    .join(".");
    lines.push(format!(
        "{{\"in\":\"{}\",\"values\":\"{values}\"}}",
        hex(&input)
    ));

    check_suite("protocol_marshal.jsonl", &lines);
}

#[test]
fn encode_limits_match_the_cpp_buffers() {
    // A message passes the 10 240-byte buffer as the C++ encoder reports
    // 0x16 for it.
    let big = CommonMessage::SetMissionLog(game_sim::protocol::common::SetMissionLog {
        data: vec![0; 0x2800],
    });
    let mut writer = WireWriter::with_limit(game_sim::protocol::ENCODE_LIMIT);
    let error = big.encode(&WireContext::CLIENT, &mut writer).unwrap_err();
    assert_eq!(error.legacy_code(), Some(0x16));
}
