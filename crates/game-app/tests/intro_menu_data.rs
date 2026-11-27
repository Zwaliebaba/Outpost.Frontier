//! The main menu's column (`game_app::intro::type_select`) on the original
//! data in `outdir/`, and, with `OUTPOST_CLIENT_ORACLE_DIR` set to a C++
//! client-oracle run, its nodes against the column in the run's first two
//! tree dumps (the slide at frame 31 and at rest at frame 92; `rtti` left
//! out, decision O53 (4)).
//!
//! The captions' box height is the "Normal" font's line height, which only
//! the Windows font backend gives; the `h` fields are left out here and the
//! port's own client run compares them.

use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::intro::type_select::{TypeSelect, build_column, make_column};
use game_app::oracle::json::OracleJson;
use game_app::oracle::tree::{FlatNode, flatten_from, node_fields, node_text};
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};

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
fn catalogs() -> Option<TextCatalogs> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    if !dir.is_dir() {
        println!("notice: outdir/ has no text; nothing was built");
        return None;
    }
    let mut catalogs = TextCatalogs::new();
    for module in CLIENT_MODULES {
        let text = fs::read_to_string(dir.join(format!("{module}.toml"))).ok();
        catalogs.register(text.as_deref().map(catalog));
    }
    Some(catalogs)
}

fn ui() -> Option<Ui> {
    let mut ui = Ui::new(Surface::new(1024, 768));
    ui.catalogs = Some(Rc::new(catalogs()?));
    Some(ui)
}

/// The line without `rtti` and without the boxes' heights.
fn comparable(line: &str) -> String {
    let mut line = line.to_owned();
    if let Some(start) = line.find(",\"rtti\":") {
        let end = line[start + 1..]
            .find(",\"name\":")
            .map_or(line.len(), |i| start + 1 + i);
        line.replace_range(start..end, "");
    }
    if let Some(start) = line.find(",\"h\":\"") {
        line.replace_range(start..start + 15, "");
    }
    line
}

/// The column made at frame 1 and updated by the frames up to `frame`.
fn column_at(ui: &mut Ui, frame: u32) -> neuron_client::ui::tree::NodeId {
    let column = make_column(ui);
    // Entered in frame 1, at the x87's 24 bits (D1400).
    build_column(ui, column, true);
    for f in 2..frame {
        // Frame 2 still runs at the x87's 24 bits (D1400).
        if let Some(c) = ui.widget_mut::<TypeSelect>(column) {
            c.single_precision = f == 2;
        }
        ui.update_all(1.0 / 30.0);
    }
    column
}

#[test]
fn the_column_has_its_meshes_captions_and_version() {
    let Some(mut ui) = ui() else { return };
    let column = column_at(&mut ui, 92);
    let nodes = flatten_from(&ui, column);
    assert_eq!(nodes.len(), 12);
    let captions: Vec<String> = nodes[6..11]
        .iter()
        .map(|n| {
            node_text(&ui, n.id)
                .map(String::from_utf16_lossy)
                .unwrap_or_default()
        })
        .collect();
    assert_eq!(
        captions,
        ["NEW GAME", "LOAD GAME", "MULTIPLAYER", "OPTIONS", "EXIT"]
    );
    let version = node_text(&ui, nodes[11].id).map(String::from_utf16_lossy);
    assert_eq!(version.as_deref(), Some("VER. 1.0"));
    // A click on Load Game slides the column out; its second hidden frame
    // hands the menu state 3.
    let load = nodes[7].id;
    game_app::oracle::tree::click(&mut ui, "text:LOAD GAME").expect("clicked");
    let mut frames = 0;
    while ui.widget::<TypeSelect>(column).unwrap().next_state == 0 && frames < 60 {
        ui.update_all(1.0 / 30.0);
        frames += 1;
    }
    assert_eq!(ui.widget::<TypeSelect>(column).unwrap().next_state, 3);
    assert!(
        ui.node(load)
            .is_some_and(|n| !n.is(neuron_client::ui::tree::VISIBLE))
    );
}

#[test]
fn the_column_equals_the_cpp_clients_first_dumps() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    for (dump, frame) in [(1u32, 31u32), (2, 92)] {
        let Some(mut ui) = ui() else { return };
        let prefix = format!("{{\"dump\":{dump},\"i\":");
        let lines: Vec<&str> = text.lines().filter(|l| l.starts_with(&prefix)).collect();
        let at = lines
            .iter()
            .position(|l| l.contains("\"p\":-1") && l.contains("\"class\":null"))
            .expect("the column in the dump");
        let base = i32::try_from(at).unwrap();
        let column = column_at(&mut ui, frame);
        let port: Vec<String> = flatten_from(&ui, column)
            .iter()
            .enumerate()
            .map(|(i, n)| {
                let placed = FlatNode {
                    parent: if n.parent < 0 { -1 } else { n.parent + base },
                    ..*n
                };
                let mut j = OracleJson::new();
                j.uint("dump", dump);
                node_fields(&mut j, &ui, &placed, base + i32::try_from(i).unwrap());
                comparable(&j.finish())
            })
            .collect();
        let cpp: Vec<String> = lines[at..at + port.len()]
            .iter()
            .map(|l| comparable(l))
            .collect();
        if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| c != p) {
            panic!(
                "dump {dump} node {i} differs:\n  C++:  {}\n  port: {}",
                cpp[i], port[i]
            );
        }
        println!(
            "tree.jsonl dump {dump}: the column's {} nodes equal",
            port.len()
        );
    }
}
