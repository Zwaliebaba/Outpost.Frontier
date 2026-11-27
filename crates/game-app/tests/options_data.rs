//! The main menu's Options page (`game_app::intro::options`, menu state 10)
//! and its preferences window (`game_app::nn_preferences`) on the original
//! data in `outdir/`, and, with `OUTPOST_CLIENT_ORACLE_DIR` set to a C++
//! client-oracle run, the page's node and the window's subtree against the
//! run's tree dumps 7 (frame 307, sliding in), 8 (frame 338, at rest) and 9
//! (frame 398, after `USER_CANCEL` at frame 338); the state is entered at
//! frame 277 (`rtti` and the text windows' font-dependent heights left
//! out, decision O53 (4)).
//!
//! The C++ run's client: the display 1024 x 768 x 32 (`OracleForceDisplayMode`),
//! a `PerfOptions.ini` of version 5 with no options (the client-oracle job's),
//! so every option is applied at 0, and a US keyboard layout. The device's
//! modes and its memory are this test's: the window shows only the display's
//! mode from them.

use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use game_app::intro::menu::{MenuHost, MenuNav};
use game_app::intro::options::{PageOptions, enter_options_page, make_page};
use game_app::keymap::names::KeyNames;
use game_app::keymap::{KEY_MAP_PATH, KeyMap};
use game_app::nn_preferences::{DeviceMode, DisplayMode, NnPreferences, PreferencesContext};
use game_app::oracle::json::OracleJson;
use game_app::oracle::tree::{FlatNode, flatten_from, node_fields};
use game_app::perf_options::PerfOptions;
use neuron_client::ui::controls::button::ButtonWidget;
use neuron_client::ui::services::UiServices;
use neuron_client::ui::tree::{NodeId, Ui, VISIBLE};
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};
use neuron_core::vfs::Host;

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

/// The frame the C++ run enters menu state 10 in.
const ENTER_FRAME: u32 = 277;
/// The frame of the C++ run's `command USER_CANCEL`.
const CANCEL_FRAME: u32 = 338;

/// The page's node and the window's subtree.
const NODES: usize = 1 + 261;

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

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

/// The names a US keyboard layout gives the keys (`GetKeyNameTextW`).
fn us_key_names() -> KeyNames {
    let mut names = KeyNames::default();
    for c in b'A'..=b'Z' {
        names.keys.insert(u32::from(c), vec![u16::from(c)]);
    }
    for c in b'0'..=b'9' {
        names.keys.insert(u32::from(c), vec![u16::from(c)]);
    }
    for (vk, name) in [
        (0x08, "Backspace"),
        (0x09, "Tab"),
        (0x0d, "Enter"),
        (0x10, "Shift"),
        (0x11, "Ctrl"),
        (0x12, "Alt"),
        (0x14, "Caps Lock"),
        (0x1b, "Esc"),
        (0x20, "Space"),
        (0x21, "Page Up"),
        (0x22, "Page Down"),
        (0x23, "End"),
        (0x24, "Home"),
        (0x25, "Left"),
        (0x26, "Up"),
        (0x27, "Right"),
        (0x28, "Down"),
        (0x2d, "Insert"),
        (0x2e, "Delete"),
    ] {
        names.keys.insert(vk, wide(name));
    }
    for (i, n) in (0x70..=0x7b).enumerate() {
        names.keys.insert(n, wide(&format!("F{}", i + 1)));
    }
    names.modifiers = [Some(wide("Shift")), Some(wide("Ctrl")), Some(wide("Alt"))];
    names
}

/// The C++ run's client as the window reads it.
fn context(ui: &mut Ui) -> PreferencesContext {
    let mut keymap = KeyMap::new();
    let map = ui.services.data_file(KEY_MAP_PATH.as_bytes());
    keymap.load_base(map.as_deref(), KEY_MAP_PATH);
    let mut perf = PerfOptions::default();
    let file = b"[PerfVersion]\nversion=5\nSkipMachineWarnings=TRUE\n".to_vec();
    assert!(perf.load(Some(file), None).is_some(), "a current file");
    let mode = |width, height, bpp| DeviceMode {
        width,
        height,
        bpp,
        unknown_format: false,
    };
    PreferencesContext {
        keymap,
        key_names: us_key_names(),
        perf,
        display: DisplayMode {
            width: 1024,
            height: 768,
            color_bpp: 32,
        },
        device_modes: vec![
            mode(800, 600, 16),
            mode(800, 600, 32),
            mode(1024, 768, 16),
            mode(1024, 768, 32),
        ],
        desktop: (1024, 768),
        video_memory: 256 << 20,
        gamma_ability: 1,
        environment_map_ability: 1,
        single_player: true,
        max_submitted_sounds: 32,
    }
}

/// The menu's host: state 10's page, the main menu's column for state 1.
struct Menu {
    page: Option<NodeId>,
    context: PreferencesContext,
}

impl MenuHost for Menu {
    fn note_menu_screen(&mut self, _state: i32) {}
    fn refresh_loading_progress(&mut self) {}
    fn single_player(&self) -> bool {
        true
    }
    fn show_cursor(&mut self) {}
    fn single_precision(&self) -> bool {
        false
    }
    fn enter_load_game(&mut self, _ui: &mut Ui) -> Option<NodeId> {
        None
    }
    fn enter_options(&mut self, ui: &mut Ui) -> NodeId {
        let page = *self.page.get_or_insert_with(|| make_page(ui));
        enter_options_page(ui, page, self.context.clone());
        page
    }
    fn leave_intro_base(&mut self) {}
    fn start_loading_screen(&mut self) {}
    fn first_settled_frame(&mut self, _ui: &mut Ui) {}
}

/// The menu in state 10 after its entry frame, then `updates` frames of
/// 1/30 s (the driver and the interface each frame).
fn entered(ui: &mut Ui, updates: u32) -> (MenuNav, Menu) {
    let context = context(ui);
    let mut host = Menu {
        page: None,
        context,
    };
    let mut nav = MenuNav {
        current: 10,
        flag_22c: true,
        ..MenuNav::default()
    };
    nav.driver_update(ui, &mut host);
    assert_eq!(nav.current, 10);
    run(ui, &mut nav, &mut host, updates);
    (nav, host)
}

fn run(ui: &mut Ui, nav: &mut MenuNav, host: &mut Menu, frames: u32) {
    for _ in 0..frames {
        ui.update_all(1.0 / 30.0);
        nav.driver_update(ui, host);
    }
}

fn window(ui: &Ui, nav: &MenuNav) -> NodeId {
    let page = nav.options_page.expect("the page");
    ui.widget::<PageOptions>(page)
        .and_then(|p| p.preferences)
        .expect("the window")
}

fn find(ui: &Ui, root: NodeId, name: &str) -> NodeId {
    flatten_from(ui, root)
        .iter()
        .map(|n| n.id)
        .find(|&id| ui.node(id).is_some_and(|n| n.name() == name.as_bytes()))
        .expect(name)
}

/// The button's activation (a release over it): the window hears 0x26.
fn activate(ui: &mut Ui, win: NodeId, name: &str) {
    let button = find(ui, win, name);
    ui.with_widget(button, (), |w, ui| {
        let b = w.as_any_mut().downcast_mut::<ButtonWidget>().unwrap();
        b.button.activate(ui, button);
    });
}

fn visible(ui: &Ui, id: NodeId) -> bool {
    ui.node(id).is_some_and(|n| n.is(VISIBLE))
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

#[test]
fn the_window_slides_in_and_main_menu_returns_to_state_one() {
    let Some(mut ui) = ui() else { return };
    let (mut nav, mut host) = entered(&mut ui, 0);
    let page = nav.options_page.unwrap();
    let win = window(&ui, &nav);
    assert_eq!(flatten_from(&ui, page).len(), 1, "the page has no children");
    assert_eq!(flatten_from(&ui, win).len(), NODES - 1);
    let title = find(&ui, win, "Screen Title");
    assert!(!visible(&ui, title));

    run(&mut ui, &mut nav, &mut host, 60);
    assert!(visible(&ui, title), "the general page is shown");
    let w = ui.widget::<NnPreferences>(win).unwrap();
    assert_eq!((w.state.cur_page, w.state.anim), (1, 1.0));

    // The cancel key does nothing here.
    let p = ui.widget_mut::<PageOptions>(page).unwrap();
    assert!(!p.on_event(15));

    // Controls: the general page left, the box turned half a turn (10.8
    // degrees a frame), then the key map's first group shown.
    activate(&mut ui, win, "keymapButton");
    let row = find(&ui, win, "ICommand_Row0");
    assert!(!visible(&ui, title) && !visible(&ui, row));
    run(&mut ui, &mut nav, &mut host, 16);
    assert!(ui.widget::<NnPreferences>(win).unwrap().state.flipping);
    run(&mut ui, &mut nav, &mut host, 1);
    let w = ui.widget::<NnPreferences>(win).unwrap();
    assert!(!w.state.flipping && w.state.cur_page == 0);
    assert!(visible(&ui, title) && visible(&ui, row));

    activate(&mut ui, win, "MainMenuButton");
    let w = ui.widget::<NnPreferences>(win).unwrap();
    assert!(w.state.closing_anim && w.state.save_perf_options_requested);
    let mut frames = 0;
    while nav.current == 10 && frames < 120 {
        run(&mut ui, &mut nav, &mut host, 1);
        frames += 1;
    }
    assert_eq!(nav.current, 1);
    assert!(!ui.contains(win), "the closed window is deleted");
    // 0 down to below -1 at 0.045 a frame, then the poll.
    assert_eq!(frames, 23);
    nav.driver_update(&mut ui, &mut host);
    assert_eq!(nav.prev, 1);
}

#[test]
fn the_page_equals_the_cpp_clients_dumps() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    for (dump, frame) in [(7u32, 307u32), (8, 338), (9, 398)] {
        let Some(mut ui) = ui() else { return };
        let prefix = format!("{{\"dump\":{dump},\"i\":");
        let lines: Vec<&str> = text.lines().filter(|l| l.starts_with(&prefix)).collect();
        let at = lines
            .iter()
            .position(|l| l.contains("\"rtti\":\"class PageOptions\""))
            .expect("the page in the dump");
        let cpp: Vec<String> = lines[at..].iter().map(|l| comparable(l)).collect();

        let before = frame.min(CANCEL_FRAME) - ENTER_FRAME - 1;
        let (mut nav, mut host) = entered(&mut ui, before);
        if frame > CANCEL_FRAME {
            // `HudDispatchCommand(USER_CANCEL)`: the page's `OnEvent(15)`.
            let page = nav.options_page.unwrap();
            let p = ui.widget_mut::<PageOptions>(page).unwrap();
            assert!(!p.on_event(15), "the page takes no cancel");
            run(&mut ui, &mut nav, &mut host, frame - CANCEL_FRAME);
        }
        let page = nav.options_page.unwrap();
        let mut flat = flatten_from(&ui, page);
        flat.extend(flatten_from(&ui, window(&ui, &nav)));
        let base = i32::try_from(at).unwrap();
        let mut root = 0;
        let port: Vec<String> = flat
            .iter()
            .enumerate()
            .map(|(i, n)| {
                let i = i32::try_from(i).unwrap();
                if n.parent < 0 {
                    root = i;
                }
                let placed = FlatNode {
                    parent: if n.parent < 0 {
                        -1
                    } else {
                        n.parent + base + root
                    },
                    ..*n
                };
                let mut j = OracleJson::new();
                j.uint("dump", dump);
                node_fields(&mut j, &ui, &placed, base + i);
                comparable(&j.finish())
            })
            .collect();
        assert_eq!(cpp.len(), NODES, "dump {dump}: the C++ nodes");
        assert_eq!(port.len(), NODES, "dump {dump}: the port's nodes");
        let differing: Vec<usize> = (0..NODES).filter(|&i| cpp[i] != port[i]).collect();
        for &i in differing.iter().take(6) {
            println!(
                "dump {dump} node {i}:\n  C++:  {}\n  port: {}",
                cpp[i], port[i]
            );
        }
        assert!(
            differing.is_empty(),
            "dump {dump}: {} of {NODES} nodes differ",
            differing.len()
        );
        println!("tree.jsonl dump {dump}: the page's and the window's {NODES} nodes equal");
    }
}
