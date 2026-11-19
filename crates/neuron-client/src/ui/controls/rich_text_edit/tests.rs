use std::cell::RefCell;

use super::*;
use crate::text::font::fake::FakeBackend;
use crate::text::font::{FontEnv, FontSystem};
use crate::text::layout::device::RichSurface;
use crate::ui::immediate::{Call, ImmediateLog};
use crate::ui::quad_batch::QuadBatcher;
use crate::ui::services::UiServices;
use crate::ui::widescreen::Surface;

fn units(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn fonts() -> FontSystem {
    let mut sys = FontSystem::new(Box::new(FakeBackend::default()), FontEnv::new(800, 600));
    sys.rich.initialize(sys.backend.as_mut(), 600);
    sys.rich.reinitialize_fonts(sys.backend.as_mut());
    sys
}

/// The game's IME box as a recording stand-in: a key or character the
/// box takes hands back a composed text.
struct Ime(Rc<RefCell<Vec<String>>>);

impl ImeBox for Ime {
    fn handle_key(&mut self, key: u32) -> Option<Vec<u16>> {
        self.0.borrow_mut().push(format!("key {key:x}"));
        (key == 0x0D).then(|| units("composed"))
    }
    fn handle_char(&mut self, byte: u8) -> Option<Vec<u16>> {
        self.0.borrow_mut().push(format!("char {byte:x}"));
        None
    }
    fn resize(&mut self, buffer_size: u32, rect: [i32; 4]) {
        self.0
            .borrow_mut()
            .push(format!("resize {buffer_size} {rect:?}"));
    }
    fn take_focus(&mut self) {
        self.0.borrow_mut().push("focus".to_owned());
    }
}

/// Services with an IME focus and an input focus.
struct Services {
    log: Rc<RefCell<Vec<String>>>,
    focus: Option<NodeId>,
}

impl UiServices for Services {
    fn input_focus(&mut self) -> Option<NodeId> {
        self.focus
    }
    fn push_input_focus(&mut self, node: NodeId) {
        self.focus = Some(node);
    }
    fn pop_input_focus(&mut self, _node: NodeId) {
        self.focus = None;
    }
    fn create_ime_box(&mut self, _owner: NodeId) -> Option<Box<dyn ImeBox>> {
        self.log.borrow_mut().push("new box".to_owned());
        Some(Box::new(Ime(self.log.clone())))
    }
    fn clear_ime_focus(&mut self) {
        self.log.borrow_mut().push("no focus".to_owned());
    }
}

struct Host {
    quads: QuadBatcher,
    imm: ImmediateLog,
}

impl DrawHost for Host {
    fn quads(&mut self) -> &mut QuadBatcher {
        &mut self.quads
    }
    fn immediate(&mut self) -> Option<&mut dyn crate::ui::immediate::Immediate> {
        Some(&mut self.imm)
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

fn key(key: u32, modifiers: u32) -> HudRecord {
    HudRecord {
        key,
        modifiers,
        down: true,
        ..HudRecord::default()
    }
}

fn ch(c: char) -> HudRecord {
    HudRecord {
        wparam: u32::from(c),
        ..HudRecord::default()
    }
}

#[test]
fn characters_runs_and_the_caret() {
    let mut ui = Ui::new(Surface::new(640, 480));
    let id = ui
        .create_by_class(Some(b"E"), b"RichTextEditWin", None, &mut Payload::None)
        .unwrap();
    with(&mut ui, id, |e, ui| {
        for c in "ab".chars() {
            assert!(e.on_char(ui, id, &ch(c)));
        }
        assert!(e.on_key_down(ui, id, &key(0x42, 4)));
        assert!(e.on_char(ui, id, &ch('c')));
        assert_eq!(e.text(), units("abc"));
        assert_eq!(e.caret, 3);
        // Two runs behind one attribute node each.
        assert_eq!(
            e.text_display.nodes,
            vec![
                RdlNode::Attributes(TextAttributes::new(EDIT_DEFAULT_ATTRIBUTES, !0, 0)),
                RdlNode::Text(units("ab")),
                RdlNode::Attributes(TextAttributes::new(EDIT_DEFAULT_ATTRIBUTES | 1, !0, 0)),
                RdlNode::Text(units("c")),
            ]
        );
        assert_eq!(e.win.content.nodes.len(), 2);
        // The caret takes the attributes of the cell before it.
        e.set_caret(1);
        assert_eq!(e.current_attrs, EDIT_DEFAULT_ATTRIBUTES);
        assert!(e.on_key_down(ui, id, &key(0x2E, 0)));
        assert_eq!(e.text(), units("ac"));
        e.set_password_mode(true);
        assert_eq!(e.text_display.nodes[1], RdlNode::Text(units("*")));
        assert_eq!(e.get_text(2), (units("a"), 2));
    });
}

#[test]
fn an_ime_box_takes_the_keys_and_hands_back_its_text() {
    let mut ui = Ui::new(Surface::new(640, 480));
    let log = Rc::new(RefCell::new(Vec::new()));
    ui.services = Box::new(Services {
        log: log.clone(),
        focus: None,
    });
    let id = ui
        .create_by_class(Some(b"E"), b"RichTextEditWin", None, &mut Payload::None)
        .unwrap();
    with(&mut ui, id, |e, ui| {
        assert!(e.ime_box.is_some());
        e.activate(ui, id);
        // With a box the caret keys are the box's.
        assert!(!e.on_key_down(ui, id, &key(0x25, 0)));
        assert!(e.on_char(ui, id, &ch('x')));
        // Enter: the box's composed text becomes the box's text.
        assert!(e.on_key_down(ui, id, &key(0x0D, 0)));
        assert_eq!(e.text(), units("composed"));
        e.deactivate(ui, id, true);
        e.set_ime_enabled(ui, id, false);
        assert!(e.ime_box.is_none());
    });
    assert_eq!(
        *log.borrow(),
        vec!["new box", "focus", "char 78", "key d", "no focus"]
    );
}

#[test]
fn the_caret_is_a_line_at_the_laid_out_character() {
    let mut ui = Ui::new(Surface::new(640, 480));
    ui.fonts = Some(fonts());
    let log = Rc::new(RefCell::new(Vec::new()));
    ui.services = Box::new(Services {
        log: log.clone(),
        focus: None,
    });
    let id = ui
        .create_by_class(Some(b"E"), b"RichTextEditWin", None, &mut Payload::None)
        .unwrap();
    let mut host = Host {
        quads: QuadBatcher::new(),
        imm: ImmediateLog::default(),
    };
    with(&mut ui, id, |e, ui| {
        e.set_ime_enabled(ui, id, false);
        e.set_text(&units("Hello"), None);
        e.activate(ui, id);
        let base = &mut e.win.base;
        base.pixel_width = 200;
        base.pixel_height = 40;
        base.screen_rect = [100, 50, 300, 90];
        base.surface = Some(RichSurface::new(256, 64));
        let content = (&e.win.content, e.win.layout_flag);
        let mut fonts = ui.fonts.take().unwrap();
        e.win
            .base
            .rebuild_layout_if_dirty(&mut fonts, Some(content));
        ui.fonts = Some(fonts);
        let (x, y, length) = e.caret_line(ui).unwrap();
        // After the fifth character, two pixels below the top, four shorter
        // than the cell.
        assert!(x > 102.0 && x < 300.0, "{x}");
        assert_eq!(y, 52.0);
        assert!(length > 0.0);
        e.set_caret(0);
        let (x0, _, _) = e.caret_line(ui).unwrap();
        assert_eq!(x0, 102.0);
        e.draw(ui, id, &mut host);
    });
    let calls = &host.imm.calls;
    assert_eq!(calls[0], Call::SetDepthWrite(false));
    assert!(calls.contains(&Call::Color4ubv([0xFF; 4])));
    assert!(calls.contains(&Call::Begin(primitive::LINES)));
    assert!(calls.contains(&Call::Vertex(102.0, 52.0, 0.0)));
    assert_eq!(calls.last(), Some(&Call::SetDepthWrite(true)));
}

#[test]
fn the_caret_blinks_with_its_period() {
    let mut e = RichTextEditWin::new();
    e.blink(0.2);
    assert!(e.caret_visible);
    assert_eq!(e.caret_timer, 0.3);
    e.blink(1.3);
    // 0.3 - 1.3 = -1.0 (and the float's residue): flipped, then two
    // periods added.
    assert!(!e.caret_visible);
    let expected = ((f64::from(0.3f32) - 1.3 + 0.5) + 0.5) as f32;
    assert_eq!(e.caret_timer, expected);
    assert!(e.caret_timer.abs() < 1e-6);
}

#[test]
fn a_page_edit_field_is_read_only_with_its_caption() {
    let mut ui = Ui::new(Surface::new(640, 480));
    let payload = EntryPayload::Text {
        right: 0.5,
        bottom: 0.04,
        caption: units("Player"),
    };
    let id = ui
        .create_by_class(
            Some(b"Name"),
            b"UITextEdit",
            None,
            &mut Payload::Page(&payload),
        )
        .unwrap();
    let w = ui.widget::<UiTextEdit>(id).unwrap();
    assert_eq!(w.edit.text(), units("Player"));
    assert!(!w.edit.editable);
    assert_eq!((w.local_width, w.edit.win.base.norm_width), (0.5, 0.5));
}

fn with<R>(ui: &mut Ui, id: NodeId, f: impl FnOnce(&mut RichTextEditWin, &mut Ui) -> R) -> R {
    let mut out = None;
    ui.with_widget(id, (), |w, ui| {
        out = Some(f(
            w.as_any_mut().downcast_mut::<RichTextEditWin>().unwrap(),
            ui,
        ));
    });
    out.unwrap()
}
