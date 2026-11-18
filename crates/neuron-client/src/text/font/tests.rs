//! The font model over the fake backend.

use std::cell::RefCell;
use std::rc::Rc;

use super::atlas::coverage_texel;
use super::config::{TrueTypeFont, read_fonts_ini};
use super::fake::{FakeBackend, FakeLog};
use super::number::GLYPH_COUNT;
use super::quad::{QuadCall, QuadRecorder};
use super::rich::RichFonts;
use super::string::{Child, DrawOrigin, FontString, FontStringChild, PaintCx, word_wrap};
use super::*;
use crate::render::texture::{Abilities, ContractPipeline, PipelineCall};

fn w(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

fn entry(nickname: &str, face: &str, fixed_height: f32) -> TrueTypeFont {
    TrueTypeFont {
        nickname: nickname.as_bytes().to_vec(),
        font: face.as_bytes().to_vec(),
        fixed_height,
    }
}

fn system(width: i32, height: i32, fonts: &[TrueTypeFont]) -> (FontSystem, Rc<RefCell<FakeLog>>) {
    let (fake, log) = FakeBackend::with_log();
    let mut sys = FontSystem::new(Box::new(fake), FontEnv::new(width, height));
    assert_eq!(sys.load_fonts(fonts), fonts.len());
    (sys, log)
}

/// A system with one font of an 18-pixel cell at 800 x 600: advances 9,
/// 4 for `i l . , ' |` and space, 18 for `M W`.
fn system18() -> (FontSystem, Rc<RefCell<FakeLog>>, FontId) {
    let (sys, log) = system(800, 600, &[entry("ListText", "Arial", 0.03)]);
    assert_eq!(sys.fonts[0].realized_height(), 18);
    (sys, log, FontId(0))
}

fn pipe() -> ContractPipeline {
    ContractPipeline::new(Abilities::default())
}

/// The heights the autoscale search settles on, against an independent
/// simulation of `SearchScaleDelta` over the fake's widths.
#[test]
fn autoscale_heights() {
    let cases: [(f32, i32, i32, i32); 12] = [
        (0.0435, 800, 600, 26),
        (0.0435, 1024, 768, 33),
        (0.0435, 1280, 960, 41),
        (0.0435, 1280, 720, 31),
        (0.0435, 1600, 1200, 51),
        (0.03, 640, 480, 15),
        (0.03, 1280, 720, 21),
        (0.03, 1600, 1200, 35),
        (0.06, 1024, 768, 47),
        (0.06, 1280, 960, 57),
        (0.06, 1280, 720, 43),
        (0.06, 1600, 1200, 72),
    ];
    for (fixed, width, height, expected) in cases {
        let (sys, _) = system(width, height, &[entry("F", "Agency FB", fixed)]);
        assert_eq!(
            sys.fonts[0].realized_height(),
            expected,
            "{fixed} at {width} x {height}"
        );
    }
}

#[test]
fn the_search_is_cached_per_surface_width() {
    let (mut sys, log) = system(800, 600, &[entry("F", "Agency FB", 0.0435)]);
    let created = log.borrow().created.len();
    sys.device_reset();
    let (font, mut cx) = sys.font_cx(FontId(0)).unwrap();
    assert_eq!(font.line_height(&mut cx), 26);
    assert_eq!(log.borrow().created.len(), created);
    sys.env.surface_width = 1024;
    sys.env.surface_height = 768;
    sys.device_reset();
    let (font, mut cx) = sys.font_cx(FontId(0)).unwrap();
    assert_eq!(font.line_height(&mut cx), 33);
    assert!(log.borrow().created.len() > created);
    assert_eq!(font.init_line_height(), 26);
}

#[test]
fn fonts_ini_names_and_lookup() {
    let ini = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../outdir/data/FONTS/fonts.ini"
    ))
    .unwrap();
    let entries = read_fonts_ini(&ini);
    assert_eq!(entries.len(), 21);
    assert_eq!(entries[4].nickname, b"ListText");
    assert_eq!(entries[4].font, b"Arial Unicode MS");
    let (sys, _) = system(1024, 768, &entries);
    assert_eq!(sys.find_font(b"listtext"), Some(FontId(4)));
    assert_eq!(sys.find_font(b"Missing"), None);
    assert_eq!(sys.fonts[4].id(), b"Arial Unicode MS,0");
    let debug = sys.find_font(b"DebugText").unwrap();
    assert_eq!(sys.fonts[debug.0].face_name(), b"FixedSys");
}

#[test]
fn measure_and_probe() {
    let (mut sys, _, id) = system18();
    let (font, mut cx) = sys.font_cx(id).unwrap();
    assert_eq!(font.measure_string(&mut cx, Some(&w("Hello"))), (35, 18));
    assert_eq!(font.measure_string(&mut cx, Some(&[])), (0, 18));
    assert_eq!(font.measure_string(&mut cx, None), (0, 0));
    assert_eq!(font.probe_string(&mut cx, 20, &w("Hello")), 2);
    assert_eq!(font.probe_string(&mut cx, 18, &w("Hello")), 2);
    assert_eq!(font.probe_string(&mut cx, 0, &w("Hello")), 0);
    assert_eq!(font.probe_string(&mut cx, -1, &w("Hello")), 5);
    let mut widths = [0; 3];
    assert_eq!(font.char_widths(&mut cx, &w("Mi\0"), &mut widths), 22);
    assert_eq!(widths, [18, 4, 0]);
    assert_eq!(font.glyph_advance(&mut cx, u16::from(b'W')), 18.0);
}

#[test]
fn word_wrap_as_the_reference_breaks() {
    let (mut sys, _, id) = system18();
    let lines = word_wrap(&mut sys, id, &w("aa bb cc"), 40.0, 0x400);
    assert_eq!(lines, vec![w("aa bb "), w("cc")]);
    let lines = word_wrap(&mut sys, id, &w("ab\r\ncd"), 100.0, 0x400);
    assert_eq!(lines, vec![w("ab"), w(""), w("cd")]);
    let lines = word_wrap(&mut sys, id, &w("aaaaaa"), 20.0, 0x400);
    assert_eq!(lines, vec![w("aa"), w("aa"), w("aa")]);
    assert_eq!(word_wrap(&mut sys, id, &w("aa bb cc"), 40.0, 1).len(), 1);
    // A character wider than the width takes a line by itself (D293).
    let lines = word_wrap(&mut sys, id, &w("MaM"), 10.0, 0x400);
    assert_eq!(lines, vec![w("M"), w("a"), w("M")]);
}

#[test]
fn rasterise_into_one_page() {
    let (mut sys, log, id) = system18();
    let mut pipe = pipe();
    let mut child = FontStringChild::new();
    child.bind_font(&mut sys, Some(id));
    child.set_text(&mut sys, Some(&w("Hello")));
    assert_eq!(child.get_size(&mut sys), (35, 18));
    assert!(child.rasterize(&mut sys, &mut pipe, true));
    let grid = child.grid().unwrap().clone();
    assert_eq!(grid.rows.len(), 1);
    let page = sys.atlas.page(grid.rows[0][0]);
    assert_eq!((page.page_w, page.page_h), (64, 32));
    assert_eq!((page.used_w, page.used_h, page.content_h), (35, 18, 18));
    let texels = page.texels();
    assert_eq!(texels[0], coverage_texel(128));
    assert_eq!(texels[1], 0xFFFF);
    assert_eq!(texels[8], 0);
    assert_eq!(texels[14 * 64 + 1], 0);
    assert!(matches!(
        pipe.calls[0],
        PipelineCall::Create {
            width: 64,
            height: 32,
            ..
        }
    ));
    assert!(matches!(
        pipe.calls[1],
        PipelineCall::Level {
            width: 64,
            height: 32,
            pitch: 128,
            result: 0,
            ..
        }
    ));
    assert_eq!(log.borrow().draws[0], (0, 0, 35, 18, w("Hello")));
    // Current: nothing drawn again.
    let draws = log.borrow().draws.len();
    assert!(child.rasterize(&mut sys, &mut pipe, true));
    assert_eq!(log.borrow().draws.len(), draws);
    // A new text gives the page back; the next grid takes it again.
    child.set_text(&mut sys, Some(&w("Hi")));
    assert_eq!(sys.atlas.free_pages().len(), 1);
    assert!(child.rasterize(&mut sys, &mut pipe, false));
    assert_eq!(child.grid().unwrap().rows[0][0], grid.rows[0][0]);
    assert_eq!(sys.atlas.free_pages().len(), 0);
}

#[test]
fn wide_strings_take_several_tiles() {
    let (mut sys, log, id) = system18();
    let mut pipe = pipe();
    let mut child = FontStringChild::new();
    child.bind_font(&mut sys, Some(id));
    child.set_text(&mut sys, Some(&w(&"M".repeat(20))));
    assert_eq!(child.get_size(&mut sys), (360, 18));
    // With the size check a 512-pixel string does not fit the scratch.
    assert!(!child.rasterize(&mut sys, &mut pipe, false));
    assert!(child.rasterize(&mut sys, &mut pipe, true));
    let grid = child.grid().unwrap();
    let pages: Vec<_> = grid.rows[0]
        .iter()
        .map(|&p| sys.atlas.page(p).clone())
        .collect();
    assert_eq!(
        (pages[0].page_w, pages[0].used_w, pages[0].src_x),
        (256, 256, 0)
    );
    assert_eq!(
        (pages[1].page_w, pages[1].used_w, pages[1].src_x),
        (128, 104, 256)
    );
    let draws = &log.borrow().draws;
    assert_eq!((draws[0].0, draws[0].2), (0, 256));
    assert_eq!((draws[1].0, draws[1].2), (-256, 104));
}

fn paint_child(
    sys: &mut FontSystem,
    child: &mut FontStringChild,
    origin: DrawOrigin,
    trims: (f32, f32),
) -> QuadRecorder {
    let mut pipe = pipe();
    let mut sink = QuadRecorder::default();
    let mut cx = PaintCx {
        pipeline: &mut pipe,
        sink: &mut sink,
        text_enabled: true,
    };
    child.paint(sys, &mut cx, &origin, trims.0, trims.1);
    sink
}

#[test]
fn paint_quads_and_shadow() {
    let (mut sys, _, id) = system18();
    sys.atlas.draw_color = 0xFF11_2233;
    let mut child = FontStringChild::new();
    child.bind_font(&mut sys, Some(id));
    child.set_text(&mut sys, Some(&w("Hi")));
    let origin = DrawOrigin {
        x: 10.7,
        y: 20.2,
        ..DrawOrigin::default()
    };
    let sink = paint_child(&mut sys, &mut child, origin, (0.0, 0.0));
    assert_eq!(sink.calls[0], QuadCall::MaterialKey(sys.atlas.material_key));
    assert!(matches!(
        sink.calls[1],
        QuadCall::Frame {
            blend_mode: 2,
            find: false,
            ..
        }
    ));
    let records = sink.records();
    assert_eq!(records.len(), 2);
    let (shadow, main) = (records[0], records[1]);
    assert_eq!(
        (shadow.x0, shadow.y0, shadow.x1, shadow.y1),
        (10.5, 20.5, 23.5, 38.5)
    );
    assert_eq!(shadow.color, 0xFF08_0808);
    assert_eq!(
        (main.x0, main.y0, main.x1, main.y1),
        (9.5, 19.5, 22.5, 37.5)
    );
    assert_eq!(
        (main.u0, main.v0, main.u1, main.v1),
        (0.0, 0.0, 0.8125, 0.5625)
    );
    assert_eq!(main.color, 0xFF33_2211);
    assert_eq!((main.depth_a, main.depth_b), (1.0, 1.0));

    // A right trim narrows the window.
    let sink = paint_child(&mut sys, &mut child, origin, (0.0, 5.0));
    let main = sink.records()[1];
    assert_eq!(main.x1, 18.0);
    assert!(main.u1 < 0.8125 && main.u1 > 0.5);

    // No shadow, and nothing at all with the text option off.
    child.set_shadow(false);
    assert_eq!(
        paint_child(&mut sys, &mut child, origin, (0.0, 0.0))
            .records()
            .len(),
        1
    );
    let mut pipe = pipe();
    let mut sink = QuadRecorder::default();
    let mut cx = PaintCx {
        pipeline: &mut pipe,
        sink: &mut sink,
        text_enabled: false,
    };
    child.paint(&mut sys, &mut cx, &origin, 0.0, 0.0);
    assert!(sink.calls.is_empty());
}

#[test]
fn multi_line_strings_wrap_and_align() {
    let (mut sys, _, id) = system18();
    sys.atlas.draw_color = 0xFF11_2233;
    let mut pipe = pipe();
    let mut s = FontString::new();
    s.multi_line = true;
    s.set_font(&mut sys, Some(id));
    s.set_text(&mut sys, Some(&w("aa bb cc")));
    let mut sink = QuadRecorder::default();
    let mut origin = DrawOrigin {
        x: 100.0,
        y: 50.0,
        width: 40.0,
        height: 100.0,
        h_mode: 1,
        v_mode: 0,
    };
    {
        let mut cx = PaintCx {
            pipeline: &mut pipe,
            sink: &mut sink,
            text_enabled: true,
        };
        s.draw_children(&mut sys, &mut cx, &mut origin, 0.0, 0.0);
    }
    assert_eq!(s.child_count(&mut sys, &mut pipe), 2);
    assert_eq!(s.extent(&mut sys, &mut pipe), (40, 36));
    let texts: Vec<_> = s
        .children()
        .iter()
        .map(|slot| match &slot.child {
            Some(Child::Text(c)) => c.text().unwrap().to_vec(),
            _ => Vec::new(),
        })
        .collect();
    assert_eq!(texts, vec![w("aa bb"), w("cc")]);
    let mains: Vec<_> = sink
        .records()
        .into_iter()
        .filter(|r| r.color != 0xFF08_0808)
        .collect();
    assert_eq!((mains[0].x0, mains[0].y0), (99.5, 49.5));
    assert_eq!((mains[1].x0, mains[1].y0), (110.5, 67.5));

    // One line: the whole text when a character fits the key.
    let mut single = FontString::new();
    single.set_font(&mut sys, Some(id));
    single.set_text(&mut sys, Some(&w("aa bb cc")));
    assert_eq!(single.extent(&mut sys, &mut pipe), (62, 18));
    // The kind byte stays when a child of the other kind replaces one.
    let at = single.ensure_child(&mut sys, 0, 1);
    assert_eq!(at, 0);
    assert!(matches!(single.children()[0].child, Some(Child::Number(_))));
    assert_eq!(single.children()[0].kind, 0);
    // Pages of released children go back to the pool.
    s.set_text(&mut sys, Some(&w("other")));
    assert_eq!(sys.atlas.free_pages().len(), 2);
}

#[test]
fn number_strings_from_their_face() {
    let (mut sys, _, id) = system18();
    sys.atlas.draw_color = 0xFFFF_FFFF;
    let mut pipe = pipe();
    let mut s = FontString::new();
    s.set_font(&mut sys, Some(id));
    s.set_number_text(&mut sys, &mut pipe, b"12%%");
    assert_eq!(s.extent(&mut sys, &mut pipe), (27, 18));
    let face = &sys.number_faces[&id];
    assert_eq!(
        face.advance,
        [9, 4, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 18, 9, 9, 9, 0]
    );
    assert_eq!(face.uv[3].u0, 22.0 / 256.0);
    assert_eq!(face.uv[3].u1, 31.0 / 256.0);
    assert_eq!(face.uv[GLYPH_COUNT - 1].v1, 0.5625);
    assert_eq!(face.ref_count, 1);

    let mut sink = QuadRecorder::default();
    let Some(Child::Number(n)) = s.child_mut(0) else {
        panic!("no number child");
    };
    let mut cx = PaintCx {
        pipeline: &mut pipe,
        sink: &mut sink,
        text_enabled: true,
    };
    let origin = DrawOrigin {
        x: 100.0,
        y: 50.0,
        ..DrawOrigin::default()
    };
    n.paint(&mut sys, &mut cx, &origin);
    assert!(matches!(sink.calls[1], QuadCall::Frame { find: true, .. }));
    let records = sink.records();
    assert_eq!(records.len(), 6);
    assert_eq!(
        (records[0].x0, records[0].y0, records[0].x1, records[0].y1),
        (100.5, 50.5, 109.5, 68.5)
    );
    assert_eq!(
        (records[1].x0, records[1].y0, records[1].x1, records[1].y1),
        (99.5, 49.5, 108.5, 67.5)
    );
    assert_eq!((records[1].u0, records[1].u1), (22.0 / 256.0, 31.0 / 256.0));
    assert_eq!((records[1].v0, records[1].v1), (0.0, 0.5625));
    assert_eq!(records[5].x1, 126.5);

    // The face goes away with its last reference; its page to the pool.
    s.release(&mut sys);
    assert!(sys.number_faces.is_empty());
    assert!(!sys.atlas.free_pages().is_empty());
}

#[test]
fn device_reset_drops_number_glyphs() {
    let (mut sys, _, id) = system18();
    let mut pipe = pipe();
    let mut s = FontString::new();
    s.set_font(&mut sys, Some(id));
    s.set_number_text(&mut sys, &mut pipe, b"5");
    assert!(sys.number_faces[&id].glyph.is_some());
    sys.device_reset();
    assert!(sys.number_faces[&id].glyph.is_none());
    assert_eq!(sys.env.layout_generation, 2);
    assert_eq!(s.extent(&mut sys, &mut pipe), (9, 18));
    sys.shutdown(&mut pipe);
    assert!(sys.fonts.is_empty());
}

#[test]
fn rich_font_tables() {
    let (mut fake, log) = FakeBackend::with_log();
    let mut rich = RichFonts::new();
    assert!(rich.initialize(&mut fake, 768));
    assert_eq!(
        (rich.scale_x, rich.scale_y, rich.drop_shadow),
        (1.0, 1.28, (2, 2))
    );
    assert!(rich.initialize(&mut fake, 600));
    assert_eq!(rich.drop_shadow, (1, 1));
    rich.reinitialize_fonts(&mut fake);
    assert_eq!(log.borrow().created.len(), 256);
    // Standard font 0 is Arial Unicode MS 22: trunc(22 * 0.78125) = 17.
    assert_eq!(rich.font(0).log_font.height, 17);
    assert_eq!(rich.font(1).log_font.weight, FW_BOLD);
    assert_eq!(rich.font(2).log_font.italic, 1);
    assert_eq!(rich.font(4).log_font.underline, 1);
    assert_eq!(rich.cell_height(0), 17);
    assert_eq!(rich.cell_width(0), 17);
    assert_eq!(rich.font_metrics(0), (13, 4));
    let mut size = rich::VisualSize::default();
    rich.text_visual_size(&mut fake, 0, Some(&w("ab")), &mut size);
    assert_eq!((size.width, size.height, size.ascent), (16, 17, 13));
    assert_eq!(rich.text_fit(&mut fake, 0, Some(&w("abc")), 16), 2);
    assert_eq!(rich.text_fit(&mut fake, 0, Some(&w("abc")), 15), 1);
    assert_eq!(rich.text_advance(&mut fake, 0, &w("abc")), 24);
    rich.shutdown(&mut fake);
    assert_eq!(log.borrow().deleted, 256);
}
