use neuron_core::image::FormatMaps;
use neuron_core::math::{Matrix, Transform, Vector};

use super::*;
use crate::render::texture::pipeline::{Abilities, ContractPipeline};
use crate::text::font::fake::FakeBackend;
use crate::text::font::{FontEnv, FontSystem};
use crate::text::rdl::RdlNode;

fn fonts() -> FontSystem {
    let mut sys = FontSystem::new(Box::new(FakeBackend::default()), FontEnv::new(800, 600));
    sys.rich.initialize(sys.backend.as_mut(), 600);
    sys.rich.reinitialize_fonts(sys.backend.as_mut());
    sys
}

fn units(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// A projection that mirrors x and y, so the reference's `1 - ndc` and
/// `ndc + 1` put the control's top left above and left of its bottom right.
const MIRROR: [f32; 16] = [
    -1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
];

fn placed() -> Transform {
    Transform {
        rotation: Matrix::IDENTITY,
        translation: Vector {
            x: -0.5,
            y: 0.5,
            z: 0.0,
        },
    }
}

#[test]
fn surface_sizes_round_up() {
    assert_eq!(round_surface_size(-3), 1);
    assert_eq!(round_surface_size(0), 0);
    assert_eq!(round_surface_size(1), 1);
    assert_eq!(round_surface_size(64), 64);
    assert_eq!(round_surface_size(65), 128);
    assert_eq!(round_surface_size(160), 256);
}

#[test]
fn scroll_origin_moves_only_forward_of_zero() {
    let mut w = BaseRichTextWin::new();
    w.surface_dirty = false;
    w.set_scroll_origin(-1, -1);
    assert!(!w.surface_dirty);
    w.set_scroll_origin(5, 7);
    assert_eq!((w.scroll_x, w.scroll_y), (5, 7));
    assert_eq!((w.trc.state.origin_h, w.trc.state.origin_v), (-5, -7));
    assert!(w.surface_dirty);
    w.set_scroll_origin(5, -2);
    assert_eq!(w.scroll_y, 7);
    w.pixel_height = 100;
    w.layout_height = 160;
    for (mode, y) in [
        (0, Some(7)),
        (1, Some(0)),
        (2, Some(30)),
        (3, Some(60)),
        (4, None),
    ] {
        w.align_mode = mode;
        assert_eq!(w.scroll_origin(), (5, y));
    }
    w.scroll_to_end();
    assert_eq!(w.scroll_y, 60);
    w.scroll_to_top();
    assert_eq!(w.scroll_y, 0);
    w.layout_dirty = false;
    w.surface_dirty = false;
    w.mark_dirty(false, true);
    assert!(w.layout_dirty && w.surface_dirty);
}

#[test]
fn draw_lays_out_renders_and_uploads() {
    let mut sys = fonts();
    let mut pipeline = ContractPipeline::new(Abilities {
        square_only: 0,
        max_width: 256,
        max_height: 256,
    });
    let formats = FormatMaps::defaults();
    let mut tiles = Vec::new();
    let mut win = RichTextWin::new();
    win.base.create_reset();
    win.set_text_content(
        &units("Hello rich text, wrapped over the width\0ignored"),
        0.4,
        0.3,
    );
    {
        let mut cx = RichTextCx {
            viewport: [0, 0, 800, 600],
            projection: MIRROR,
            pipeline: &mut pipeline,
            formats: &formats,
            tiles: &mut tiles,
        };
        win.draw_with(&placed(), &mut sys, &mut cx);
    }
    let b = &win.base;
    assert_eq!(b.screen_rect, [200, 150, 360, 240]);
    assert_eq!((b.pixel_width, b.pixel_height), (160, 90));
    let surface = b.surface.as_ref().unwrap();
    assert_eq!((surface.width, surface.height, b.pitch), (256, 128, 512));
    assert!(!b.layout_dirty);
    assert!(!b.surface_dirty, "the upload clears the surface flag");
    // Two lines at the fake's 22-pixel cell (Arial Unicode MS 22 at a
    // scale of 600 / 768, truncated to 17), each behind a position node.
    assert!(b.layout_height > 17);
    assert!(matches!(b.layout_list.nodes[0], RdlNode::Position(_)));
    assert!(surface.pixels.contains(&0x00FF_FFFF));
    assert!(surface.pixels.contains(&0x000A_0A0A));
    assert!(!tiles.is_empty());
    // Nothing is drawn again while neither flag is up.
    let before = surface.pixels.clone();
    win.base
        .render(&mut sys, Some((&win.content, true)), &mut pipeline);
    assert_eq!(win.base.surface.as_ref().unwrap().pixels, before);
}

#[test]
fn layout_keeps_the_end_in_view_and_makes_the_bar() {
    let mut sys = fonts();
    let mut pipeline = ContractPipeline::new(Abilities {
        square_only: 0,
        max_width: 512,
        max_height: 512,
    });
    let formats = FormatMaps::defaults();
    let mut tiles = Vec::new();
    let mut win = RichTextWin::new();
    win.base.create_reset();
    win.base.set_auto_scrollbar(true);
    win.base.keep_scrolled_to_end = true;
    for line in 0..12 {
        win.append_node(&RdlNode::Text(units(&format!("line {line}"))));
        win.append_node(&RdlNode::Paragraph);
    }
    let mut cx = RichTextCx {
        viewport: [0, 0, 800, 600],
        projection: MIRROR,
        pipeline: &mut pipeline,
        formats: &formats,
        tiles: &mut tiles,
    };
    win.draw_with(&placed(), &mut sys, &mut cx);
    let b = &win.base;
    assert!(b.layout_height > b.pixel_height);
    let bar = b.scroll_bar_v.unwrap();
    assert_eq!(bar.max, b.layout_height as f32);
    assert_eq!(bar.page, 90.0);
    // More than 60 pixels from the end: the view stays at the top.
    let overflow = b.layout_height - b.pixel_height;
    assert!(overflow > 60);
    assert_eq!(b.scroll_y, 0);
    // Within 60 pixels of it, a new line keeps the end in view.
    win.on_scroll_bar((overflow - 10) as f32 + 0.9);
    assert_eq!(win.base.scroll_y, overflow - 10);
    win.append_node(&RdlNode::Text(units("one more")));
    win.draw_with(&placed(), &mut sys, &mut cx);
    let b = &win.base;
    assert_eq!(b.scroll_y, b.layout_height - b.pixel_height);
    assert!(b.scroll_y > overflow);
    assert_eq!(b.trc.state.origin_v, -b.scroll_y);
    assert_eq!(b.scroll_bar_v.unwrap().value, b.scroll_y as f32);
}
