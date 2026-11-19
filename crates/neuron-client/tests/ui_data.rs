//! The shipped interface files (`outdir/data`, decision O15) through the UI
//! toolkit of work package P4.13: every page descriptor loads and becomes a
//! page whose widgets sit inside the 4:3 interface on a 4:3 and on a wide
//! surface (70 of the 72 widgets with the toolkit's own controls), the
//! `[BaseFrame]` file builds its buttons through the factory, and
//! `mouse.ini` gives its shapes and cursors. Nothing is written
//! or compared with the oracle; the page descriptor loader and the
//! screen-quad batcher are compared with the C++ in `oracle_vectors.rs`.

use std::fs;
use std::path::{Path, PathBuf};

use std::cell::RefCell;
use std::rc::Rc;

use neuron_client::ui::control::{ControlMesh, MeshLoader, PlainControl, load_ini};
use neuron_client::ui::controls::button::ButtonWidget;
use neuron_client::ui::cursor::{Cursors, compute_quad};
use neuron_client::ui::factory::{CLASSES, owner};
use neuron_client::ui::immediate::{Immediate, ImmediateLog};
use neuron_client::ui::page::{EntryPayload, MasterWin, PageDesc, Style};
use neuron_client::ui::quad_batch::QuadBatcher;
use neuron_client::ui::shape::FixedTextures;
use neuron_client::ui::tree::DrawHost;
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector};

fn data() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data")
}

fn pages() -> Vec<PathBuf> {
    let dir = data().join("INTERFACE/INTRO");
    let mut out: Vec<PathBuf> = fs::read_dir(&dir)
        .map(|entries| {
            entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| {
                    p.extension()
                        .is_some_and(|e| e.eq_ignore_ascii_case("page"))
                })
                .collect()
        })
        .unwrap_or_default();
    out.sort();
    out
}

/// A mesh instance that only exists.
struct NoMesh;

impl ControlMesh for NoMesh {
    fn transform(&self) -> Transform {
        Transform::IDENTITY
    }
    fn set_position(&mut self, _: Vector) {}
    fn set_orientation(&mut self, _: Matrix) {}
    fn set_transform(&mut self, _: Transform) {}
    fn hardpoint(&self, _: &[u8]) -> Option<Transform> {
        None
    }
}

/// A loader that gives every path an instance and keeps the paths.
struct PathLoader(Rc<RefCell<Vec<Vec<u8>>>>);

impl MeshLoader for PathLoader {
    fn load(&mut self, path: &[u8], _: &[u8]) -> Option<Box<dyn ControlMesh>> {
        self.0.borrow_mut().push(path.to_vec());
        Some(Box::new(NoMesh))
    }
}

/// A draw host that records the immediate-mode calls.
#[derive(Default)]
struct LogHost {
    quads: QuadBatcher,
    log: ImmediateLog,
    meshes: usize,
}

impl DrawHost for LogHost {
    fn quads(&mut self) -> &mut QuadBatcher {
        &mut self.quads
    }
    fn immediate(&mut self) -> Option<&mut dyn Immediate> {
        Some(&mut self.log)
    }
    fn submit_mesh(&mut self, _: &dyn ControlMesh, _: neuron_client::ui::tree::MeshPass) {
        self.meshes += 1;
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
}

/// A manager whose factory has every class of the reference: the toolkit's
/// own, and a bare control for each class not ported yet.
fn ui_with_every_class(surface: Surface) -> Ui {
    let mut ui = Ui::new(surface);
    for (name, _) in CLASSES {
        if ui.factory.find(name.as_bytes()).is_none() {
            ui.factory
                .register(name, || Box::new(PlainControl::default()));
        }
    }
    ui
}

#[test]
fn shipped_pages_load_and_place_their_widgets() {
    let files = pages();
    if files.is_empty() {
        println!("outdir/data has no page descriptors: nothing to load");
        return;
    }
    let mut entries = 0;
    let ported = [
        "UIStaticText",
        "UIListBox",
        "UIScroll",
        "UICycle",
        "UIWinObject",
        "UIRichTextWin",
        "UITextMsgButton",
        "UITextEdit",
        "UIFastTable",
    ];
    let (mut built, mut immediate_calls, mut meshes) = (0, 0, 0);
    for path in &files {
        let bytes = fs::read(path).unwrap();
        let desc = PageDesc::load(&bytes);
        assert!(desc.ok, "{}", path.display());
        for e in &desc.entries {
            assert!(
                owner(&e.type_name).is_some(),
                "{}: unknown class {}",
                path.display(),
                String::from_utf8_lossy(&e.type_name)
            );
        }
        entries += desc.entries.len();
        let mut names: Vec<&[u8]> = desc.entries.iter().map(|e| e.name.as_slice()).collect();
        names.sort_unstable();
        names.dedup();

        for (surface, left, right) in [
            (Surface::new(1024, 768), 0.0, 1024.0),
            (Surface::new(1920, 1080), 240.0, 1680.0),
        ] {
            let mut ui = ui_with_every_class(surface);
            let loaded = Rc::new(RefCell::new(Vec::new()));
            ui.mesh_loader = Some(Box::new(PathLoader(loaded.clone())));
            let id = MasterWin::open(&mut ui, &desc)
                .unwrap_or_else(|| panic!("{}: the page failed", path.display()));
            let page = ui.widget::<MasterWin>(id).unwrap().page.clone().unwrap();
            assert_eq!(page.count(), names.len(), "{}", path.display());
            // The panels' meshes and the text buttons' style meshes.
            let objects = desc
                .entries
                .iter()
                .filter(|e| {
                    e.type_name == b"UIWinObject"
                        || matches!(
                            &e.payload,
                            EntryPayload::TextMsgButton {
                                style: Style::Default | Style::Named(_),
                                ..
                            }
                        )
                })
                .count();
            assert_eq!(loaded.borrow().len(), objects, "{}", path.display());
            for i in 0..page.count() {
                let widget = page.widget(i).unwrap();
                let class = ui.node(widget).unwrap().class_name.unwrap();
                if ported.contains(&class) {
                    built += usize::from(surface.width == 1024);
                }
            }
            let mut host = LogHost::default();
            ui.update_all(0.016);
            ui.draw_all(&mut host);
            if surface.width == 1024 {
                immediate_calls += host.log.calls.len();
                meshes += host.meshes;
            }
            for i in 0..page.count() {
                let widget = page.widget(i).unwrap();
                let position = ui.node(widget).unwrap().position;
                let (x, y, _) = ui.camera.project(position).unwrap();
                let slack = 2.0;
                assert!(
                    x >= left - slack
                        && x <= right + slack
                        && y >= -slack
                        && y <= surface.height as f32 + slack,
                    "{} {}: ({x}, {y}) is off the interface",
                    path.display(),
                    String::from_utf8_lossy(page.widget_name(i).unwrap())
                );
            }
            ui.destroy(id);
            ui.delete(id);
            assert_eq!(ui.live_count(), 0);
        }
    }
    assert!(built > 0 && immediate_calls > 0 && meshes > 0);
    println!(
        "{} pages, {entries} entries, {built} built by the toolkit's own controls, \
         {immediate_calls} immediate-mode calls and {meshes} meshes drawn at 1024 x 768",
        files.len()
    );
}

#[test]
fn the_navigation_bar_builds_its_controls() {
    let Ok(text) = fs::read(data().join("INTERFACE/BASESIDE/navbar.ini")) else {
        println!("outdir/data has no navbar.ini: nothing to load");
        return;
    };
    let mut ui = ui_with_every_class(Surface::new(1024, 768));
    let loaded = Rc::new(RefCell::new(Vec::new()));
    ui.mesh_loader = Some(Box::new(PathLoader(loaded.clone())));
    let id = ui
        .create_by_class(
            Some(b"NavBar"),
            b"WinControl",
            None,
            &mut neuron_client::ui::tree::Payload::None,
        )
        .unwrap();
    let mut ini = IniReader::new();
    assert!(ini.open_memory(&text));
    load_ini(&mut ui, id, &mut ini);
    let control = ui.win_control(id).unwrap();
    assert_eq!(control.offset.y, f32::from_bits(0x3EB3_3333));
    assert_eq!(control.offset.z, -1.0);
    let children = ui.children(id);
    assert_eq!(children.len(), 12);
    for &child in &children {
        let node = ui.node(child).unwrap();
        assert_eq!(node.class_name, Some("ButtonControl"));
        assert!(
            node.name().starts_with(b"RoomControl") || node.name().starts_with(b"ButtonControl")
        );
        // The toolkit's own button, its mesh loaded by its INI section.
        let button = &ui.widget::<ButtonWidget>(child).unwrap().button;
        assert!(button.control.mesh.is_some());
    }
    assert_eq!(loaded.borrow().len(), 12);
    let mut host = LogHost::default();
    ui.update_all(0.016);
    ui.draw_all(&mut host);
    assert!(host.meshes >= 12);
    println!(
        "navbar.ini: {} buttons built by the toolkit, {} mesh passes drawn",
        children.len(),
        host.meshes
    );
}

#[test]
fn mouse_ini_gives_shapes_and_cursors() {
    let Ok(text) = fs::read(data().join("mouse.ini")) else {
        println!("outdir/data has no mouse.ini: nothing to load");
        return;
    };
    let mut lib = FixedTextures {
        size: (256, 256),
        ..FixedTextures::default()
    };
    let mut cursors = Cursors::new();
    cursors.init(&text, &mut lib);
    assert_eq!(lib.files, vec![b"screens\\cursor.TXM".to_vec()]);
    assert_eq!(cursors.shapes.len(), 16);
    assert_eq!(cursors.cursors.len(), 19);
    let names: Vec<Vec<u8>> = cursors.cursors.iter().map(|c| c.name.clone()).collect();
    let surface = Surface::new(1920, 1080);
    for name in &names {
        cursors.set_cursor(Some(name), false);
        let shape = cursors
            .animate(0.0)
            .unwrap_or_else(|| panic!("cursor {}: no shape", String::from_utf8_lossy(name)));
        assert_eq!((shape.pixel_w, shape.pixel_h), (64, 64));
        let cursor = cursors.current().unwrap().clone();
        let quad = compute_quad(&cursor, &shape, 500, 400, &surface);
        assert!(quad.iter().all(|v| v.is_finite()));
    }
    cursors.set_cursor(Some(b"arrow"), true);
    assert!(cursors.is_arrow());
}
