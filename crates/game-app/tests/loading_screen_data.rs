//! The loading screen's progress bar (`game_app::loading_screen`) on the
//! original data in `outdir/`, and, with `OUTPOST_CLIENT_ORACLE_DIR` set to
//! a C++ client-oracle run, its nodes against the bar's subtree in the
//! run's first tree dump (`rtti` left out, decision O53 (4)).

use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::loading_screen::{LoadingProgressBar, LoadingScreen};
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

fn without_rtti(line: &str) -> String {
    let Some(start) = line.find(",\"rtti\":") else {
        return line.to_owned();
    };
    let end = line[start + 1..]
        .find(",\"name\":")
        .map_or(line.len(), |i| start + 1 + i);
    format!("{}{}", &line[..start], &line[end..])
}

#[test]
fn the_bar_is_made_once_hidden_with_its_caption() {
    let Some(mut ui) = ui() else { return };
    let mut screen = LoadingScreen::default();
    screen.create_progress_bar(&mut ui);
    let id = screen.progress_bar.expect("the bar");
    screen.create_progress_bar(&mut ui);
    assert_eq!(screen.progress_bar, Some(id), "made once");
    let nodes = flatten_from(&ui, id);
    assert_eq!(nodes.len(), 3);
    let text = node_text(&ui, nodes[1].id).map(String::from_utf16_lossy);
    assert_eq!(text.as_deref(), Some("LOADING"));
    assert_eq!(ui.widget::<LoadingProgressBar>(id).unwrap().period, 3.0);
    // Shown while the loading screen runs.
    screen.set_started(&mut ui, true);
    ui.update_all(1.0 / 30.0);
    assert!(nodes.iter().all(|n| {
        ui.node(n.id)
            .is_some_and(|n| n.is(neuron_client::ui::tree::VISIBLE))
    }));
}

#[test]
fn the_bar_equals_the_cpp_clients_first_dump() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let Some(mut ui) = ui() else { return };
    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let first: Vec<&str> = text
        .lines()
        .filter(|l| l.starts_with("{\"dump\":1,\"i\":"))
        .collect();
    let at = first
        .iter()
        .position(|l| l.contains("\"class\":\"LoadingProgressBar\""))
        .expect("the bar in dump 1");
    let base = i32::try_from(at).unwrap();
    let mut screen = LoadingScreen::default();
    screen.create_progress_bar(&mut ui);
    // The frames before the dump update the hidden bar (`UpdateInactive`).
    ui.update_all(1.0 / 30.0);
    let nodes = flatten_from(&ui, screen.progress_bar.expect("the bar"));
    let port: Vec<String> = nodes
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let placed = FlatNode {
                parent: if n.parent < 0 { -1 } else { n.parent + base },
                ..*n
            };
            let mut j = OracleJson::new();
            j.uint("dump", 1);
            node_fields(&mut j, &ui, &placed, base + i32::try_from(i).unwrap());
            without_rtti(&j.finish())
        })
        .collect();
    let cpp: Vec<String> = first[at..at + port.len()]
        .iter()
        .map(|l| without_rtti(l))
        .collect();
    if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| c != p) {
        panic!("node {i} differs:\n  C++:  {}\n  port: {}", cpp[i], port[i]);
    }
    println!("tree.jsonl: the bar's {} nodes equal", port.len());
}
