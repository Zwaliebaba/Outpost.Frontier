//! P4.11, P4.13, P4.16: the window's IME for the boxes the interface
//! registers. The engine's focus manager runs on the IME context it is
//! given (the game window's on Windows); a rich-text edit window gets its
//! `FLImeInputBox` through the engine's services only while the context
//! has an IME; the IME's result characters reach the window's text; the
//! frame's IME pass draws the toolbar of the focused box.

use std::cell::RefCell;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use neuron_client::engine::{Engine, EngineConfig, FrameInput};
use neuron_client::render::appearance::Camera;
use neuron_client::render::ff::device::RecordingBackend;
use neuron_client::text::font::fake::FakeBackend;
use neuron_client::text_input::ime::{CaretRect, ImeContext, ImeView};
use neuron_client::ui::controls::rich_text_edit::RichTextEditWin;
use neuron_client::ui::tree::{NodeId, Payload};
use neuron_client::window::DisplayMode;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("outpost-ime-{name}-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn engine(data: &Path) -> Engine<RecordingBackend> {
    let mode = DisplayMode {
        width: 640,
        height: 480,
        fullscreen: false,
    };
    let mut config = EngineConfig::new(data, mode);
    config.font_backend = Some(Box::new(FakeBackend::default()));
    Engine::new(config, RecordingBackend::default()).expect("the engine")
}

struct Still;

impl Camera for Still {
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 640, 480]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        35.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        27.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn z_far(&mut self) -> f32 {
        1000.0
    }
    fn aspect(&mut self) -> f32 {
        4.0 / 3.0
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _: [f32; 3], _: &mut [f32; 3]) -> i32 {
        0
    }
    fn unproject(&mut self, _: &mut [f32; 3], _: f32, _: f32) {}
    fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
        0
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    }
}

/// A window's IME with an IME installed and a composition of its own.
#[derive(Default)]
struct Log {
    calls: Vec<String>,
    composition: Vec<u16>,
}

struct Scripted(Rc<RefCell<Log>>);

impl ImeContext for Scripted {
    fn set_open(&mut self, open: bool) {
        self.0.borrow_mut().calls.push(format!("open {open}"));
    }
    fn finalize(&mut self, send: bool) -> Vec<u16> {
        let text = std::mem::take(&mut self.0.borrow_mut().composition);
        if send { text } else { Vec::new() }
    }
    fn set_caret(&mut self, caret: CaretRect) {
        self.0
            .borrow_mut()
            .calls
            .push(format!("caret {} {}", caret.x, caret.y));
    }
    fn set_boxes_registered(&mut self, registered: bool) {
        self.0
            .borrow_mut()
            .calls
            .push(format!("boxes {registered}"));
    }
    fn available(&self) -> bool {
        true
    }
    fn view(&mut self) -> ImeView {
        ImeView {
            state: 1,
            language: 0x0411_0411,
            ..ImeView::default()
        }
    }
}

fn edit_window(engine: &mut Engine<RecordingBackend>) -> NodeId {
    engine
        .ui_mut()
        .unwrap()
        .create(
            Box::new(RichTextEditWin::default()),
            Some("RichTextEditWin"),
            None,
            &mut Payload::None,
        )
        .expect("the window")
}

fn text(engine: &Engine<RecordingBackend>, id: NodeId) -> String {
    let (units, _) = engine
        .ui()
        .unwrap()
        .widget::<RichTextEditWin>(id)
        .unwrap()
        .get_text(64);
    String::from_utf16_lossy(&units[..units.iter().position(|&u| u == 0).unwrap_or(units.len())])
}

#[test]
fn without_an_ime_the_windows_have_no_box() {
    let mut engine = engine(&scratch("none"));
    let id = edit_window(&mut engine);
    assert!(engine.ime_boxes().unwrap().is_empty());
    assert!(!engine.ime_char(u16::from(b'a')));
    assert_eq!(text(&engine, id), "");
}

#[test]
fn the_windows_ime_reaches_the_box_its_window_and_the_draw() {
    let mut engine = engine(&scratch("ime"));
    let baseline = engine
        .frame(FrameInput::idle(1.0 / 30.0), &mut Still)
        .ui_draws;
    let log = Rc::new(RefCell::new(Log::default()));
    engine.set_ime_context(Box::new(Scripted(Rc::clone(&log))));
    assert_eq!(log.borrow().calls, ["open false", "boxes false"]);
    let id = edit_window(&mut engine);
    let boxes = engine.ime_boxes().unwrap().clone();
    assert_eq!(boxes.len(), 1);
    let focus = boxes.focus().expect("the first box takes the IME focus");
    assert_eq!(boxes.owner(focus), Some(id));
    assert!(log.borrow().calls.contains(&"open true".to_owned()));

    // GSendChar: the IME's result string into the box, the box's text into
    // its window (UpdatedText).
    for unit in "ab".encode_utf16() {
        assert!(engine.ime_char(unit));
    }
    assert_eq!(text(&engine, id), "ab");
    assert_eq!(
        boxes.with_box(focus, |b| b.text().to_vec()),
        Some(vec![0x61, 0x62])
    );

    // The IME pass draws the toolbar's fan for the focused box.
    let with_box = engine
        .frame(FrameInput::idle(1.0 / 30.0), &mut Still)
        .ui_draws;
    assert!(
        with_box > baseline,
        "{with_box} draws, {baseline} without the box"
    );

    // The window loses the focus: a composition in progress goes into the
    // box and its window; nothing more is drawn.
    log.borrow_mut().composition = "c".encode_utf16().collect();
    engine.ime_window_focus(false);
    assert_eq!(text(&engine, id), "abc");
    let unfocused = engine
        .frame(FrameInput::idle(1.0 / 30.0), &mut Still)
        .ui_draws;
    assert_eq!(unfocused, baseline);

    // The window deleted: its box leaves the focus manager.
    engine.ime_window_focus(true);
    engine.ui_mut().unwrap().delete(id);
    assert!(boxes.is_empty());
    assert_eq!(boxes.focus(), None);
}
