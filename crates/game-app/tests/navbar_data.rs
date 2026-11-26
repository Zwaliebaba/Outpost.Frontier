//! The navigation bar's window tree on the original data (`outdir/`,
//! decision O15) and against the C++ client's `+oracle` file (P5.9,
//! decision O53).
//!
//! - `the_bar_builds_its_tree`: `NavBarEnsure`'s factory call and
//!   `NavBar::Create` on `navbar.ini` give the frame, the hidden tab and
//!   the twelve buttons, and the `[NavBar]` values.
//! - `win_ini_records_equal_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`):
//!   the `winini` and `winnode` records of the C++ run's `layout.jsonl` for
//!   `navbar.ini` equal the port's, one bar per load (`menu_walk.txt` has
//!   one, at frame 0; `single_player.txt` a second, in the first base),
//!   with the C++ record's frame and path, leaving out `rtti` (O53 (4)).
//!   Without the variable it prints a notice.

use std::path::PathBuf;

use game_app::base::nav_bar::{self, NavBar};
use game_app::oracle::layout::win_ini_lines;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::tree::{NodeId, Ui};
use neuron_client::ui::widescreen::Surface;
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// The data tree's files for the controls (`GetFileSysPath` and
/// `INI_Reader::open`).
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
    ui.services = Box::new(DataFiles(data_dir()));
    nav_bar::register(&mut ui.factory);
    Some(ui)
}

/// The bar made by `NavBarEnsure`'s factory call; the file names of the
/// `LoadIni` calls the hook saw, with their controls.
fn build(ui: &mut Ui) -> (NodeId, Vec<(NodeId, Vec<u8>)>) {
    let loads = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
    let seen = std::rc::Rc::clone(&loads);
    ui.ini_loaded = Some(Box::new(move |_: &Ui, id, name: &[u8]| {
        seen.borrow_mut().push((id, name.to_vec()));
    }));
    let id = nav_bar::ensure(ui).expect("the bar");
    ui.ini_loaded = None;
    let loads = loads.borrow().clone();
    (id, loads)
}

#[test]
fn the_bar_builds_its_tree() {
    let Some(mut ui) = ui() else { return };
    let (id, loads) = build(&mut ui);
    assert_eq!(
        loads,
        vec![(id, nav_bar::NAVBAR_INI.to_vec())],
        "navbar.ini loads"
    );
    assert_eq!(ui.node(id).unwrap().name(), b"NavBar");
    assert_eq!(ui.node(id).unwrap().class_name, Some("NavBar"));
    assert_eq!(nav_bar::ensure(&mut ui), Some(id), "found the second time");
    let names: Vec<Vec<u8>> = ui
        .children(id)
        .iter()
        .map(|&c| ui.node(c).unwrap().name().to_vec())
        .collect();
    assert_eq!(names.len(), 14);
    assert_eq!(names[0], b"ManeuverFrame");
    assert_eq!(names[1], b"ManeuverTab");
    assert_eq!(names[2], b"RoomControl1");
    assert_eq!(names[13], b"ButtonControl5");
    let bar = ui.widget::<NavBar>(id).unwrap();
    assert_eq!(bar.ini_values.len(), 26);
    assert_eq!(
        bar.ini_values
            .get(b"IDS_HOTSPOT_LAUNCH".as_slice())
            .map(Vec::as_slice),
        Some(b"interface\\baseside\\take_off.3db".as_slice())
    );
    assert_eq!(bar.default_mesh, Some(nav_bar::DEFAULT_MESH));
}

/// The record without its `rtti` field, which O53 (4) leaves out.
fn without_rtti(line: &str) -> String {
    let Some(start) = line.find(",\"rtti\":") else {
        return line.to_owned();
    };
    let end = line[start + 1..]
        .find(",\"name\":")
        .map_or(line.len(), |i| start + 1 + i);
    format!("{}{}", &line[..start], &line[end..])
}

fn frame(line: &str) -> u32 {
    let rest = &line[line.find("\"frame\":").map_or(0, |i| i + 8)..];
    rest.split(',')
        .next()
        .and_then(|f| f.parse().ok())
        .unwrap_or(0)
}

#[test]
fn win_ini_records_equal_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let Some(mut ui) = ui() else { return };
    let text = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let cpp: Vec<String> = text
        .lines()
        .filter(|l| {
            (l.contains("\"rec\":\"winini\"") || l.contains("\"rec\":\"winnode\""))
                && l.to_ascii_lowercase().contains("navbar.ini")
        })
        .map(without_rtti)
        .collect();
    assert!(!cpp.is_empty(), "the run has a navbar.ini load");
    // Each load (a bar made on entering a room, `NavBarEnsure`, after the
    // last one was destroyed) gives a fresh bar's records at its frame.
    let port = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
    for head in cpp.iter().filter(|l| l.contains("\"rec\":\"winini\"")) {
        let path_start = head.find("\"path\":\"").expect("a path") + 8;
        let path_end = path_start + head[path_start..].find('"').expect("the path's end");
        // The C++ path's escapes are `\\` only (ASCII).
        let path = head[path_start..path_end].replace("\\\\", "\\");
        let lines = std::rc::Rc::clone(&port);
        let at = frame(head);
        ui.ini_loaded = Some(Box::new(move |ui: &Ui, id, _: &[u8]| {
            lines
                .borrow_mut()
                .extend(win_ini_lines(ui, id, at, path.as_bytes()));
        }));
        let bar = nav_bar::ensure(&mut ui).expect("the bar");
        ui.destroy(bar);
        ui.delete(bar);
    }
    let port: Vec<String> = port.borrow().iter().map(|l| without_rtti(l)).collect();
    if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| c != p) {
        panic!(
            "record {} differs:\n  C++:  {}\n  port: {}",
            i + 1,
            cpp[i],
            port[i]
        );
    }
    assert_eq!(cpp.len(), port.len(), "the record counts differ");
    println!("layout.jsonl: {} navbar.ini records equal", port.len());
}
