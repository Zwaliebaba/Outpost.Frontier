//! The HUD's elements (`game_app::hud`) on the original data in `outdir/`:
//! `hud.ini` through the factories, and, with `OUTPOST_CLIENT_ORACLE_SP_DIR`
//! set to a C++ client-oracle run of `single_player.txt`, each element's
//! window against the same window in the run's tree dump 3 (the first in
//! space), and the `hud` and `huditem` records against its `layout.jsonl`.
//!
//! The windows are compared each on its own, with their nodes' indices
//! counted from the window (the dump's other roots are not all ported);
//! `rtti` is left out (decision O53 (4)), and the text windows' heights,
//! which only the Windows font backend gives. The interface first runs the
//! oracle's fixed frames from the `hud` record's frame to the dump's, as
//! the C++ client's updates did.

use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::hud::status::{
    self, NANOBOT_ARCH, SHIELD_BATTERY_ARCH, StatusInput, StatusLauncher, StatusLegalHps,
    StatusShip,
};
use game_app::hud::{self, FACTORIES};
use game_app::oracle::tree::dump_nodes;
use game_sim::data::archetype::equipment::EquipmentKind;
use game_sim::data::config::{DataConfig, GameData};
use neuron_client::ui::montage;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::shape::FixedTextures;
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

/// `hud.ini`'s path as `SetHudIniPath` stores it on the reference run
/// (`HUD = interface\hud.ini` of `[Data]`, under the data root).
const HUD_INI: &[u8] = b".\\data\\interface\\hud.ini";

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

struct DataFiles(PathBuf);

impl UiServices for DataFiles {
    fn data_file(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        let rel: Vec<u8> = path
            .iter()
            .map(|&b| if b == b'\\' { b'/' } else { b })
            .collect();
        let found = Host::new(self.0.clone()).locate(&rel).ok().flatten()?;
        std::fs::read(found.path).ok()
    }
}

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

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
    let dir = data_dir().join("text");
    let mut catalogs = TextCatalogs::new();
    for module in CLIENT_MODULES {
        let text = std::fs::read_to_string(dir.join(format!("{module}.toml"))).ok();
        catalogs.register(text.as_deref().map(catalog));
    }
    catalogs
}

/// The interface and the HUD's elements, `None` without the data.
fn hud() -> Option<(Ui, hud::HudElements, Vec<String>)> {
    if !data_dir().join("INTERFACE").is_dir() {
        println!("notice: outdir/ has no data; nothing was built");
        return None;
    }
    let mut ui = Ui::new(Surface::new(1024, 768));
    ui.catalogs = Some(Rc::new(catalogs()));
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
    let ini = files.data_file(b"interface\\hud.ini");
    ui.services = Box::new(files);
    let mut warnings = Vec::new();
    let elements = hud::load_hud_elements(&mut ui, HUD_INI, ini, FACTORIES, &mut |w| {
        warnings.push(w);
    });
    Some((ui, elements, warnings))
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

/// The new game's player in `newplayer.fl` (its `[Player]` section): the
/// launchers of its `equip` lines with their hardpoints and ammo (the
/// `cargo` line of their projectile), the ship archetype's `hp_type` lines
/// and the nanobot and battery counts, as `HUD_Status` reads them.
fn new_player_status() -> StatusInput {
    let root = data_dir().join("..");
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini").expect("the ini");
    let mut data = GameData::new();
    data.load(&Host::new(data_dir()), &config, None);
    let mut reader = IniReader::new();
    let text = std::fs::read(root.join("newplayer.fl")).expect("newplayer.fl");
    assert!(reader.open(b"newplayer.fl", text));
    let (mut ship, mut equip, mut cargo) = (None, Vec::new(), Vec::new());
    while reader.read_header() {
        if !reader.is_header(b"Player") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"ship_archetype") {
                ship = Some(create_id(&reader.value_string_at(0)));
            } else if reader.is_value(b"equip") {
                equip.push((reader.value_string_at(0), reader.value_string_at(1)));
            } else if reader.is_value(b"cargo") {
                cargo.push((create_id(&reader.value_string_at(0)), reader.value_int(1)));
            }
        }
    }
    let count = |id: u32| cargo.iter().find(|&&(c, _)| c == id).map(|&(_, n)| n);
    let archetypes = &data.archetypes;
    let mut launchers = Vec::new();
    for (sub_id, (nickname, hardpoint)) in (1u16..).zip(&equip) {
        let Some(e) = archetypes.equipment.get(&create_id(nickname)) else {
            continue;
        };
        let launcher = match &e.kind {
            EquipmentKind::Gun(g) => &g.launcher,
            EquipmentKind::MineDropper(l) => l,
            EquipmentKind::CounterMeasureDropper { launcher, .. } => launcher,
            _ => continue,
        };
        let projectile = launcher.projectile_archetype;
        let requires_ammo = archetypes
            .equipment
            .get(&projectile)
            .and_then(|p| p.projectile())
            .is_some_and(|p| p.requires_ammo);
        launchers.push(StatusLauncher {
            sub_id,
            hardpoint: hardpoint.clone(),
            ids_name: e.root.ids_name,
            ammo_count: if requires_ammo {
                count(projectile).unwrap_or(0)
            } else {
                -1
            },
            active: true,
        });
    }
    let legal_hps = ship.and_then(|id| archetypes.ships.get(&id)).map(|s| {
        s.legal_hps
            .iter()
            .map(|l| StatusLegalHps {
                hp_type: l.hp_type.cast_signed(),
                hardpoints: l
                    .hardpoints
                    .iter()
                    .map(|&h| data.strings.get(h).to_vec())
                    .collect(),
            })
            .collect()
    });
    StatusInput {
        ship: Some(StatusShip {
            launchers,
            legal_hps,
            ..StatusShip::default()
        }),
        view_object: true,
        target: true,
        repair_count: count(create_id(NANOBOT_ARCH)),
        battery_count: count(create_id(SHIELD_BATTERY_ARCH)),
        ..StatusInput::default()
    }
}

/// The text of `"key":"..."` in the first line of `text` holding `marker`.
fn quoted<'a>(text: &'a str, marker: &str, key: &str) -> &'a str {
    let line = text.lines().find(|l| l.contains(marker)).expect(marker);
    let pattern = format!("\"{key}\":\"");
    let at = line.find(&pattern).expect(key) + pattern.len();
    &line[at..at + line[at..].find('"').unwrap()]
}

/// The HUD's frames from its load (`layout.jsonl`'s `hud` record) to tree
/// dump 3 (`session.jsonl`), at the run's fixed step, with the new
/// player's status and the session flags the server sent
/// (`SendCommonSetInterfaceState`, `iclient.jsonl`).
fn run_to_dump_3(ui: &mut Ui, elements: &hud::HudElements, dir: &Path) {
    let read = |name: &str| std::fs::read_to_string(dir.join(name)).expect(name);
    let (session, layout, iclient) = (
        read("session.jsonl"),
        read("layout.jsonl"),
        read("iclient.jsonl"),
    );
    let step = quoted(&session, "\"rec\":\"start\"", "frame_seconds");
    let step = f64::from_bits(u64::from_str_radix(step, 16).unwrap());
    ui.frame_time = step;
    let frame = |text: &str, marker: &str| -> u32 {
        let line = text.lines().find(|l| l.contains(marker)).expect(marker);
        field(line, "frame").parse().unwrap()
    };
    let frames =
        frame(&session, "\"rec\":\"dump\",\"dump\":3,") - frame(&layout, "\"rec\":\"hud\"");
    let data = quoted(&iclient, "SendCommonSetInterfaceState", "Data");
    let flags = u32::from_str_radix(data, 16).unwrap().swap_bytes();
    let window = elements
        .items
        .iter()
        .find(|i| i.name() == status::ITEM_NAME)
        .and_then(|i| i.window())
        .expect("HUD_Status");
    status::set_input(
        ui,
        window,
        StatusInput {
            session_flags: flags,
            ..new_player_status()
        },
    );
    // The frames from the HUD's load to the dump: `Win3DUpdateAll` over
    // every window.
    for _ in 0..frames {
        ui.update_all(step);
    }
}

#[test]
fn hud_ini_makes_its_eleven_elements() {
    let Some((ui, elements, warnings)) = hud() else {
        return;
    };
    // Every one of the file's eleven element sections makes its element.
    assert_eq!(elements.items.len(), 11);
    assert!(warnings.is_empty(), "{warnings:#?}");
    let core = elements
        .items
        .iter()
        .find(|i| i.name() == b"Core")
        .and_then(|i| i.window())
        .expect("HUD_Core");
    assert_eq!(ui.node(core).unwrap().name(), b"HUD_Core");
}

#[test]
fn the_elements_equal_the_cpp_clients_first_dump_in_space() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_SP_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_SP_DIR is not set; nothing was compared");
        return;
    };
    let Some((mut ui, elements, _)) = hud() else {
        return;
    };
    run_to_dump_3(&mut ui, &elements, &dir);
    let text = std::fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let cpp: Vec<String> = text
        .lines()
        .filter(|l| l.starts_with("{\"dump\":3,\"i\":"))
        .map(String::from)
        .collect();
    let port = dump_nodes(&ui, 3).nodes;
    let mut compared = 0;
    let mut windows = 0;
    for item in &elements.items {
        let Some(window) = item.window() else {
            continue;
        };
        windows += 1;
        let name = String::from_utf8_lossy(ui.node(window).unwrap().name()).into_owned();
        let ours = subtree(&port, &name);
        let theirs = subtree(&cpp, &name);
        assert!(!theirs.is_empty(), "{name} is in the C++ dump");
        for (a, b) in ours.iter().zip(&theirs) {
            assert_eq!(a, b, "{name}");
        }
        assert_eq!(ours.len(), theirs.len(), "{name}'s nodes");
        compared += ours.len();
    }
    // A comparison that judges nothing fails (the conductor's rule): the
    // ten elements with a window (all but `HUD_Steer`) are compared.
    assert_eq!(windows, 10, "the HUD's windows compared");
    println!("tree.jsonl dump 3: {compared} nodes of the HUD's windows are equal");

    // `OracleOnHudElementsLoaded`: the `hud` head and every element's
    // `huditem` record, in the file's order.
    let layout = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let head = layout
        .lines()
        .find(|l| l.contains("\"rec\":\"hud\""))
        .expect("a hud record");
    let frame: u32 = field(head, "frame").parse().unwrap();
    let ours: Vec<String> = hud::oracle_lines(frame, HUD_INI, &elements)
        .iter()
        .map(|l| mask_from(l, 0, "rtti"))
        .collect();
    let theirs: Vec<String> = layout
        .lines()
        .filter(|l| {
            l.contains(&format!("{{\"frame\":{frame},"))
                && (l.contains("\"rec\":\"hud\"") || l.contains("\"rec\":\"huditem\""))
        })
        .map(|l| mask_from(l, 0, "rtti"))
        .collect();
    assert_eq!(ours, theirs);
    println!(
        "layout.jsonl: the hud record and its {} items are equal",
        ours.len() - 1
    );
}
