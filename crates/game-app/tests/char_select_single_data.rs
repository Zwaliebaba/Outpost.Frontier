//! The single-player Load Game page (`game_app::intro::char_select_single`,
//! menu state 18) on the original data in `outdir/`, and, with
//! `OUTPOST_CLIENT_ORACLE_DIR` set to a C++ client-oracle run, its nodes
//! against the page in the run's tree dumps 3 (frame 148, sliding in) and 4
//! (frame 179, at rest); the page is entered at frame 118 (`rtti` left out,
//! decision O53 (4)).
//!
//! The roster is empty here; the C++ run's has no row past the twentieth
//! either (its scroll bar is hidden).

use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::intro::char_select_single::{
    PageCharacterSelectSingle, Roster, enter_load_game_page, make_page,
};
use game_app::oracle::json::OracleJson;
use game_app::oracle::tree::{FlatNode, flatten_from, node_fields};
use neuron_client::ui::services::UiServices;
use neuron_client::ui::tree::{NodeId, Ui, VISIBLE};
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};
use neuron_core::vfs::Host;

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

/// The frame the C++ run enters menu state 18 in.
const ENTER_FRAME: u32 = 118;

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
fn catalogs() -> Option<TextCatalogs> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    if !dir.is_dir() || !data_dir().join("INTERFACE").is_dir() {
        println!("notice: outdir/ has no data; nothing was built");
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
    ui.services = Box::new(DataFiles(data_dir()));
    Some(ui)
}

/// The line without `rtti` and without the font-dependent heights of the
/// text windows.
fn comparable(line: &str) -> String {
    let mut line = line.to_owned();
    if let Some(start) = line.find(",\"rtti\":") {
        let end = line[start + 1..]
            .find(",\"name\":")
            .map_or(line.len(), |i| start + 1 + i);
        line.replace_range(start..end, "");
    }
    for key in ["\"tw\":{", "\"tc\":{"] {
        if let Some(at) = line.find(key)
            && let Some(h) = line[at..].find(",\"h\":\"")
        {
            let start = at + h;
            line.replace_range(start..start + 15, "");
        }
    }
    line
}

/// The page entered with an empty roster and updated by `updates` frames
/// of 1/30 s.
fn page_after(ui: &mut Ui, updates: u32) -> NodeId {
    let page = make_page(ui);
    let roster = Roster {
        status: 1,
        rows: Vec::new(),
    };
    assert!(enter_load_game_page(ui, page, roster), "the page loads");
    for _ in 0..updates {
        ui.update_all(1.0 / 30.0);
    }
    page
}

fn visible(ui: &Ui, id: NodeId) -> bool {
    ui.node(id).is_some_and(|n| n.is(VISIBLE))
}

#[test]
fn the_page_slides_in_and_main_menu_leaves_with_result_one() {
    let Some(mut ui) = ui() else { return };
    let page = page_after(&mut ui, 0);
    let nodes = flatten_from(&ui, page);
    assert_eq!(nodes.len(), 21, "the page, 5 chrome statics, 15 widgets");
    let p = ui.widget::<PageCharacterSelectSingle>(page).unwrap();
    let widgets = [
        p.character_money,
        p.character_rank,
        p.character_location,
        p.character_ship,
        p.load_character,
        p.delete_character,
        p.main_menu,
        p.char_scroll,
        p.char_sel_desc,
        p.char_sel_time,
        p.money_label,
        p.rank_label,
        p.location_label,
        p.ship_label,
        p.screen_title,
    ]
    .map(Option::unwrap);
    assert!(widgets.iter().all(|&w| !visible(&ui, w)));
    for _ in 0..60 {
        ui.update_all(1.0 / 30.0);
    }
    let scroll = widgets[7];
    assert!(
        widgets
            .iter()
            .filter(|&&w| w != scroll)
            .all(|&w| visible(&ui, w))
    );
    assert!(!visible(&ui, scroll), "no more than 20 rows: no bar");
    assert_eq!(
        ui.widget::<PageCharacterSelectSingle>(page)
            .unwrap()
            .slide_t,
        2.0
    );

    game_app::oracle::tree::click(&mut ui, "MainMenu").expect("clicked");
    assert!(!visible(&ui, widgets[6]));
    let mut frames = 0;
    while ui.widget::<PageCharacterSelectSingle>(page).unwrap().result == 0 && frames < 60 {
        ui.update_all(1.0 / 30.0);
        frames += 1;
    }
    let p = ui.widget::<PageCharacterSelectSingle>(page).unwrap();
    assert_eq!(p.result, 1);
    assert!(!p.leaving);
    // 0 down to below -0.5 at 1.5 per second.
    assert_eq!(frames, 11);
}

#[test]
fn the_page_equals_the_cpp_clients_dumps() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    for (dump, frame) in [(3u32, 148u32), (4, 179)] {
        let Some(mut ui) = ui() else { return };
        let prefix = format!("{{\"dump\":{dump},\"i\":");
        let lines: Vec<&str> = text.lines().filter(|l| l.starts_with(&prefix)).collect();
        let at = lines
            .iter()
            .position(|l| l.contains("\"rtti\":\"class PageCharacterSelectSingle\""))
            .expect("the page in the dump");
        let base = i32::try_from(at).unwrap();
        let page = page_after(&mut ui, frame - ENTER_FRAME - 1);
        let port: Vec<String> = flatten_from(&ui, page)
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
        let cpp: Vec<String> = lines[at..]
            .iter()
            .take_while(|l| !l.contains("\"d\":0") || l.contains("PageCharacterSelectSingle"))
            .map(|l| comparable(l))
            .collect();
        assert_eq!(cpp.len(), port.len(), "dump {dump}: the page's node count");
        if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| c != p) {
            panic!(
                "dump {dump} node {i} differs:\n  C++:  {}\n  port: {}",
                cpp[i], port[i]
            );
        }
        println!(
            "tree.jsonl dump {dump}: the page's {} nodes equal",
            port.len()
        );
    }
}
