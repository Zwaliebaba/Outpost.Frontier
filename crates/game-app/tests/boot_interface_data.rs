//! The windows `InitializeEngine` makes (`game_app::boot_interface`), on the
//! original data in `outdir/`: their order, names and classes, and, with
//! `OUTPOST_CLIENT_ORACLE_DIR` set to a C++ client-oracle run, their nodes
//! against the run's first tree dump (`rtti` left out, decision O53 (4)).
//!
//! Without the engine no mesh is loaded, so the group window's half
//! extents, which come from its mesh, are left out here; the port's own
//! client run compares them.

use std::path::PathBuf;

use game_app::boot_interface;
use game_app::oracle::tree::dump_nodes;
use neuron_client::ui::montage;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::shape::FixedTextures;
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
use neuron_core::vfs::Host;

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

fn ui() -> Option<Ui> {
    if !data_dir().join("INTERFACE").is_dir() {
        println!("notice: outdir/ has no data; nothing was built");
        return None;
    }
    let mut ui = Ui::new(Surface::new(1024, 768));
    let mut files = DataFiles(data_dir());
    // The button montage, which the engine loads at start-up: a button
    // whose mesh is a montage tile loads no mesh.
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
    boot_interface::register(&mut ui.factory);
    Some(ui)
}

/// The line without `rtti`, and without the group window's mesh half
/// extents.
fn comparable(line: &str) -> String {
    let mut line = line.to_owned();
    if let Some(start) = line.find(",\"rtti\":") {
        let end = line[start + 1..]
            .find(",\"name\":")
            .map_or(line.len(), |i| start + 1 + i);
        line.replace_range(start..end, "");
    }
    if line.contains("\"name\":\"NN_GroupControl\"")
        && let Some(start) = line.find(",\"half\":[")
    {
        let end = start + 1 + line[start + 1..].find(']').unwrap_or(0) + 1;
        line.replace_range(start..end, "");
    }
    line
}

#[test]
fn the_windows_come_in_the_engines_order() {
    let Some(mut ui) = ui() else { return };
    let made = boot_interface::create(&mut ui);
    let roots: Vec<_> = [
        made.briefing_parent,
        made.group_control,
        made.tooltip,
        made.rollover,
    ]
    .into_iter()
    .map(|id| {
        let n = ui.node(id.expect("made")).unwrap();
        (n.class_name, String::from_utf8_lossy(n.name()).into_owned())
    })
    .collect();
    assert_eq!(
        roots,
        [
            (Some("StaticControl"), "BriefingParentWindow".to_owned()),
            (Some("NN_GroupControl"), "NN_GroupControl".to_owned()),
            (Some("ToolTip"), "ToolTip".to_owned()),
            (Some("Rollover"), "Rollover".to_owned()),
        ]
    );
    let dump = dump_nodes(&ui, 1);
    assert_eq!(dump.roots, 4);
    assert_eq!(
        dump.nodes.len(),
        13,
        "the group window has 7 children and 2 captions"
    );
}

#[test]
fn the_windows_equal_the_cpp_clients_first_dump() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let Some(mut ui) = ui() else { return };
    boot_interface::create(&mut ui);
    // The frames before the dump run `Win3DUpdateAll`, whose tool-tip tick
    // leaves the hidden tip inactive.
    ui.update_all(1.0 / 30.0);
    let port: Vec<String> = dump_nodes(&ui, 1)
        .nodes
        .iter()
        .map(|l| comparable(l))
        .collect();
    let text = std::fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let cpp: Vec<String> = text
        .lines()
        .filter(|l| l.starts_with("{\"dump\":1,\"i\":"))
        .take(port.len())
        .map(comparable)
        .collect();
    if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| c != p) {
        panic!("node {i} differs:\n  C++:  {}\n  port: {}", cpp[i], port[i]);
    }
    assert_eq!(cpp.len(), port.len(), "the node counts differ");
    println!(
        "tree.jsonl: the {} nodes of the boot windows equal",
        port.len()
    );
}
