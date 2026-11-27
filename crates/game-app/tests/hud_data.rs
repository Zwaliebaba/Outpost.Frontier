//! The HUD's elements (`game_app::hud`) on the original data in `outdir/`:
//! `hud.ini` through the factories, and, with `OUTPOST_CLIENT_ORACLE_SP_DIR`
//! set to a C++ client-oracle run of `single_player.txt`, each element's
//! window against the same window in the run's tree dump 3 (the first in
//! space), and the `hud` and `huditem` records against its `layout.jsonl`.
//!
//! The windows are compared each on its own, with their nodes' indices
//! counted from the window (the dump's other roots are not all ported);
//! `rtti` is left out (decision O53 (4)), and the text windows' heights,
//! which only the Windows font backend gives.

use std::path::PathBuf;

use game_app::hud::{self, FACTORIES};
use game_app::oracle::tree::dump_nodes;
use neuron_client::ui::montage;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::shape::FixedTextures;
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
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

/// The interface and the HUD's elements, `None` without the data.
fn hud() -> Option<(Ui, hud::HudElements, Vec<String>)> {
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
    let ini = files.data_file(b"interface\\hud.ini");
    ui.services = Box::new(files);
    let mut warnings = Vec::new();
    let elements = hud::load_hud_elements(&mut ui, HUD_INI, ini, FACTORIES, &mut |w| {
        warnings.push(w);
    });
    // A frame of the interface (`Win3DUpdateAll`), as the frames between the
    // load and the dump run: the elements' `Update` with no player ship.
    ui.update_all(0.0);
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

#[test]
fn hud_ini_makes_the_ported_elements_and_warns_for_the_others() {
    let Some((ui, elements, warnings)) = hud() else {
        return;
    };
    // Every one of the file's eleven element sections makes its element or,
    // not ported yet, warns.
    assert_eq!(elements.items.len() + warnings.len(), 11, "{warnings:#?}");
    assert!(
        warnings
            .iter()
            .all(|w| w.contains("HUD - unknown INI entry")),
        "{warnings:#?}"
    );
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
    let Some((ui, elements, _)) = hud() else {
        return;
    };
    let text = std::fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let cpp: Vec<String> = text
        .lines()
        .filter(|l| l.starts_with("{\"dump\":3,\"i\":"))
        .map(String::from)
        .collect();
    let port = dump_nodes(&ui, 3).nodes;
    let mut compared = 0;
    for item in &elements.items {
        let Some(window) = item.window() else {
            continue;
        };
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
    println!("tree.jsonl dump 3: {compared} nodes of the HUD's windows are equal");

    // `OracleOnHudElementsLoaded`: the ported elements' records, by name
    // (their index waits for the other elements).
    let layout = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let frame: u32 = layout
        .lines()
        .find(|l| l.contains("\"rec\":\"hud\""))
        .map(|l| field(l, "frame").parse().unwrap())
        .expect("a hud record");
    let ours = hud::oracle_lines(frame, HUD_INI, &elements);
    let theirs: Vec<String> = layout
        .lines()
        .filter(|l| l.contains("\"rec\":\"huditem\"") && l.contains(&format!("\"frame\":{frame},")))
        .map(|l| mask_from(&mask_from(l, 0, "rtti"), 0, "i"))
        .collect();
    for line in &ours[1..] {
        let line = mask_from(&mask_from(line, 0, "rtti"), 0, "i");
        assert!(theirs.contains(&line), "{line} is one of {theirs:#?}");
    }
}
