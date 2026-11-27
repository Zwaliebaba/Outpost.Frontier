//! The NeuroNet windows the client makes on entering space
//! (`game_app::neuronet`) on the original data in `outdir/`: the bar
//! (`NnSystemInit`'s window) and the arrival caption
//! (`NnArrivalTextShowSystemNow` with the new game's start system, read
//! from `outdir/newplayer.fl` and `universe.ini`), and, with
//! `OUTPOST_CLIENT_ORACLE_SP_DIR` set to a C++ client-oracle run of
//! `single_player.txt`, each window against the same window in the run's
//! tree dump 3 (frame 148, the first in space).
//!
//! The windows are compared each on its own, with their nodes' indices
//! counted from the window (the dump's other roots are not all ported);
//! `rtti` is left out (decision O53 (4)), and the text windows' heights,
//! which only the Windows font backend gives.

use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::neuronet::{self, arrival_text, main_button};
use game_app::oracle::tree::dump_nodes;
use neuron_client::ui::montage;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::shape::FixedTextures;
use neuron_client::ui::tree::{NodeId, Ui};
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// The master copy's top (`newplayer.fl`), the data root's parent.
fn exe_dir() -> PathBuf {
    data_dir().join("..")
}

struct DataFiles(PathBuf);

impl UiServices for DataFiles {
    fn data_file(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        let rel: Vec<u8> = path
            .iter()
            .map(|&b| if b == b'\\' { b'/' } else { b })
            .collect();
        let found = Host::new(self.0.clone()).locate(&rel).ok().flatten()?;
        fs::read(found.path).ok()
    }
}

/// A module's catalog built from its TOML source as `cargo xtask catalog`
/// builds it.
fn catalog(text: &str) -> TextCatalog {
    let module: toml::Table = text.parse().expect("the module's TOML");
    let mut catalog = TextCatalog::new();
    let table = |name: &str| {
        module
            .get(name)
            .and_then(toml::Value::as_table)
            .cloned()
            .unwrap_or_default()
    };
    for (id, value) in table("strings") {
        let units = value.as_str().unwrap_or_default().encode_utf16().collect();
        catalog.add_string(id.parse().unwrap(), units).unwrap();
    }
    for (id, value) in table("cards") {
        let mut bytes = CARD_BOM.to_vec();
        bytes.extend(
            value
                .as_str()
                .unwrap_or_default()
                .encode_utf16()
                .flat_map(u16::to_le_bytes),
        );
        catalog.add_card(id.parse().unwrap(), bytes).unwrap();
    }
    catalog
}

/// The client's modules from `outdir/data/text`, in the client's order.
fn catalogs() -> TextCatalogs {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    let mut catalogs = TextCatalogs::new();
    for module in CLIENT_MODULES {
        let text = fs::read_to_string(dir.join(format!("{module}.toml"))).ok();
        catalogs.register(text.as_deref().map(catalog));
    }
    catalogs
}

/// The first value of `key` in the first `[header]` section whose `match_key`
/// is `match_value` (any value when `match_key` is empty), without case.
fn ini_value(
    text: &[u8],
    header: &[u8],
    match_key: &[u8],
    match_value: &[u8],
    key: &[u8],
) -> Option<Vec<u8>> {
    let mut reader = IniReader::new();
    if !reader.open_memory(text) {
        return None;
    }
    while reader.read_header() {
        if !reader.is_header(header) {
            continue;
        }
        let mut matched = match_key.is_empty();
        let mut found = None;
        while reader.read_value() {
            if !match_key.is_empty() && reader.is_value(match_key) {
                matched = reader.value_string().eq_ignore_ascii_case(match_value);
            }
            if found.is_none() && reader.is_value(key) {
                found = Some(reader.value_string());
            }
        }
        if matched && found.is_some() {
            return found;
        }
    }
    None
}

/// The new game's start system's `strid_name`: `newplayer.fl`'s `[Player]
/// system`, then that system's `[system] strid_name` in `universe.ini`.
fn start_system_strid() -> Option<(Vec<u8>, i32)> {
    let player = fs::read(exe_dir().join("newplayer.fl")).ok()?;
    let system = ini_value(&player, b"Player", b"", b"", b"system")?;
    let mut files = DataFiles(data_dir());
    let universe = files.data_file(b"UNIVERSE\\universe.ini")?;
    let strid = ini_value(&universe, b"system", b"nickname", &system, b"strid_name")?;
    let strid = std::str::from_utf8(&strid).ok()?.trim().parse().ok()?;
    Some((system, strid))
}

/// The windows made on entering space, `None` without the data.
fn space_windows() -> Option<(Ui, Option<NodeId>, Option<NodeId>)> {
    if !data_dir().join("INTERFACE").is_dir() {
        println!("notice: outdir/ has no data; nothing was built");
        return None;
    }
    let mut ui = Ui::new(Surface::new(1024, 768));
    let mut files = DataFiles(data_dir());
    let textures = files.data_file(montage::BUTTON_TEXTURES_INI);
    let tiles = files.data_file(montage::BUTTON_MONTAGE_INI);
    ui.montage.load(
        textures.as_deref(),
        tiles.as_deref(),
        &mut FixedTextures {
            size: (256, 256),
            ..FixedTextures::default()
        },
    );
    ui.services = Box::new(files);
    ui.catalogs = Some(Rc::new(catalogs()));
    neuronet::register(&mut ui.factory);
    let (_, strid) = start_system_strid().expect("the start system's strid_name");
    let mut bar = None;
    main_button::system_init(&mut ui, &mut bar, true, true);
    let mut caption = None;
    arrival_text::show_system_now(&mut ui, &mut caption, strid);
    Some((ui, bar, caption))
}

/// The line with the value of the first `"key":` at or after `from`
/// replaced by `"*"`.
fn mask_from(line: &str, from: usize, key: &str) -> String {
    let pattern = format!("\"{key}\":");
    let Some(i) = line[from..].find(&pattern) else {
        return line.to_owned();
    };
    let start = from + i + pattern.len();
    let bytes = line.as_bytes();
    let mut end = start;
    if bytes[start] == b'"' {
        end += 1;
        while bytes[end] != b'"' {
            end += if bytes[end] == b'\\' { 2 } else { 1 };
        }
        end += 1;
    } else {
        while !matches!(bytes[end], b',' | b'}' | b']') {
            end += 1;
        }
    }
    format!("{}\"*\"{}", &line[..start], &line[end..])
}

/// The line without `rtti` and the text windows' heights.
fn comparable(line: &str) -> String {
    let mut line = mask_from(line, 0, "rtti");
    for object in ["\"tw\":{", "\"tc\":{"] {
        if let Some(at) = line.find(object) {
            line = mask_from(&line, at, "h");
        }
    }
    line
}

/// A field's value as text (a number or a quoted string).
fn field<'a>(line: &'a str, key: &str) -> &'a str {
    let at = line.find(&format!("\"{key}\":")).expect(key) + key.len() + 3;
    let end = line[at..].find([',', '}']).map_or(line.len(), |i| at + i);
    &line[at..end]
}

/// The subtree of the root named `name` among `lines` (one dump's node
/// lines in order), with `i` and `p` counted from the root and without
/// `rtti` and the text heights.
fn subtree(lines: &[String], name: &str) -> Vec<String> {
    let start = lines
        .iter()
        .position(|l| field(l, "d") == "0" && field(l, "name") == format!("\"{name}\""));
    let Some(start) = start else {
        return Vec::new();
    };
    let root: i64 = field(&lines[start], "i").parse().unwrap();
    let mut out = Vec::new();
    for (n, line) in lines[start..].iter().enumerate() {
        if n > 0 && field(line, "d") == "0" {
            break;
        }
        let i: i64 = field(line, "i").parse().unwrap();
        let p: i64 = field(line, "p").parse().unwrap();
        let p = if p < 0 { p } else { p - root };
        let rebased = line
            .replacen(&format!("\"i\":{i},"), &format!("\"i\":{},", i - root), 1)
            .replacen(
                &format!("\"p\":{},", field(line, "p")),
                &format!("\"p\":{p},"),
                1,
            );
        out.push(comparable(&rebased));
    }
    out
}

#[test]
fn the_new_games_start_system_is_omicron_major() {
    let Some((ui, bar, caption)) = space_windows() else {
        return;
    };
    let (system, strid) = start_system_strid().unwrap();
    assert!(system.eq_ignore_ascii_case(b"relayseven_system"));
    assert_eq!(strid, 0x30031);
    assert_eq!(ui.node(bar.unwrap()).unwrap().name(), main_button::CLASS);
    let window = ui
        .widget::<arrival_text::NnArrivalText>(caption.unwrap())
        .unwrap();
    assert!(window.wait_for_letterbox);
    assert_eq!(window.delay.to_bits(), 2.0f32.to_bits());
    assert_eq!(window.hold_time.to_bits(), 2.0f32.to_bits());
    // A second init and a second show reuse the windows.
    let mut ui = ui;
    let mut again = bar;
    main_button::system_init(&mut ui, &mut again, true, true);
    assert_eq!(again, bar);
    let mut again = caption;
    arrival_text::show_system_now(&mut ui, &mut again, strid);
    assert_eq!(again, caption);
}

#[test]
fn the_windows_equal_the_cpp_clients_first_dump_in_space() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_SP_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_SP_DIR is not set; nothing was compared");
        return;
    };
    let Some((ui, _, _)) = space_windows() else {
        return;
    };
    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let cpp: Vec<String> = text
        .lines()
        .filter(|l| l.starts_with("{\"dump\":3,\"i\":"))
        .map(String::from)
        .collect();
    let port = dump_nodes(&ui, 3).nodes;
    let mut compared = 0;
    for (name, count) in [("NN_MainButton", 12), ("NN_ArrivalText", 2)] {
        let ours = subtree(&port, name);
        let theirs = subtree(&cpp, name);
        assert_eq!(theirs.len(), count, "{name} in the C++ dump");
        for (a, b) in ours.iter().zip(&theirs) {
            assert_eq!(a, b, "{name}");
        }
        assert_eq!(ours.len(), theirs.len(), "{name}'s nodes");
        compared += ours.len();
    }
    println!("tree.jsonl dump 3: {compared} nodes of the NeuroNet windows are equal");
}

/// Every `[system]`'s `strid_name` in `universe.ini`.
fn system_strids() -> Vec<u32> {
    let mut files = DataFiles(data_dir());
    let Some(universe) = files.data_file(b"UNIVERSE\\universe.ini") else {
        return Vec::new();
    };
    let mut reader = IniReader::new();
    assert!(reader.open_memory(&universe));
    let mut strids = Vec::new();
    while reader.read_header() {
        if !reader.is_header(b"system") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"strid_name") {
                strids.push(reader.value_int(0).cast_unsigned());
            }
        }
    }
    strids
}

/// D1136's condition (owner, 2026-11-27): the arrival caption's port of
/// `_swprintf` and `CharUpperW` equals the reference's only while every
/// name it formats is ASCII and every format uses only `%s` and `%%`; the
/// shipped data must keep that.
#[test]
fn the_arrival_captions_names_and_formats_are_ascii_with_only_s_and_percent() {
    if !data_dir().join("UNIVERSE").is_dir() {
        println!("notice: outdir/ has no data; nothing was checked");
        return;
    }
    let catalogs = catalogs();
    let text = |ids: u32| String::from_utf16_lossy(catalogs.string(ids));
    let strids = system_strids();
    assert!(!strids.is_empty(), "universe.ini's systems");
    let regions = [0x4e1, 0x4e2, 0x4e3, 0x4e4];
    for ids in strids.iter().copied().chain(regions) {
        let name = text(ids);
        assert!(!name.is_empty(), "name {ids:#x} is in the catalogs");
        assert!(name.is_ascii(), "name {ids:#x} {name:?} is not ASCII");
    }
    let formats = [
        arrival_text::IDS_NAMED_FORMAT,
        arrival_text::IDS_SYSTEM_FORMAT,
        arrival_text::IDS_REGION_FORMAT,
    ];
    for ids in formats {
        let format = text(ids);
        assert!(!format.is_empty(), "format {ids:#x} is in the catalogs");
        assert!(format.is_ascii(), "format {ids:#x} {format:?} is not ASCII");
        let mut chars = format.chars();
        while let Some(c) = chars.next() {
            if c == '%' {
                let code = chars.next();
                assert!(
                    matches!(code, Some('s' | '%')),
                    "format {ids:#x} {format:?} has %{code:?}"
                );
            }
        }
    }
    println!(
        "{} system names, {} region names and {} formats checked",
        strids.len(),
        regions.len(),
        formats.len()
    );
}
