use neuron_core::image::FormatMaps;
use neuron_core::math::{Matrix, Transform, Vector};

use super::*;
use crate::render::texture::big_image::TileDraw;
use crate::render::texture::pipeline::{Abilities, ContractPipeline};
use crate::text::font::fake::FakeBackend;
use crate::text::font::{FontEnv, FontSystem};
use crate::text::rdl::RdlNode;
use crate::ui::tree::MeshPass;
use crate::ui::widescreen::Surface;

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
    let mut ui = Ui::new(Surface::new(800, 600));
    let ui = &mut ui;
    let mut w = BaseRichTextWin::new();
    w.surface_dirty = false;
    w.set_scroll_origin(ui, -1, -1);
    assert!(!w.surface_dirty);
    w.set_scroll_origin(ui, 5, 7);
    assert_eq!((w.scroll_x, w.scroll_y), (5, 7));
    assert_eq!((w.trc.state.origin_h, w.trc.state.origin_v), (-5, -7));
    assert!(w.surface_dirty);
    w.set_scroll_origin(ui, 5, -2);
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
    w.scroll_to_end(ui);
    assert_eq!(w.scroll_y, 60);
    w.scroll_to_top(ui);
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

/// A host with the rich-text context of the tests and a recording
/// immediate layer.
struct TestHost {
    pipeline: ContractPipeline,
    formats: FormatMaps,
    tiles: Vec<TileDraw>,
    quads: crate::ui::quad_batch::QuadBatcher,
    calls: Vec<String>,
    meshes: Vec<MeshPass>,
}

impl TestHost {
    fn new() -> TestHost {
        TestHost {
            pipeline: ContractPipeline::new(Abilities {
                square_only: 0,
                max_width: 512,
                max_height: 512,
            }),
            formats: FormatMaps::defaults(),
            tiles: Vec::new(),
            quads: crate::ui::quad_batch::QuadBatcher::default(),
            calls: Vec::new(),
            meshes: Vec::new(),
        }
    }
}

impl crate::ui::immediate::Immediate for TestHost {
    fn begin(&mut self, primitive: u32) {
        self.calls.push(format!("begin {primitive}"));
    }
    fn end(&mut self) {
        self.calls.push("end".to_owned());
    }
    fn vertex(&mut self, x: f32, y: f32, z: f32) {
        self.calls.push(format!("v {x} {y} {z}"));
    }
    fn color3ub(&mut self, r: u8, g: u8, b: u8) {
        self.calls.push(format!("c3 {r} {g} {b}"));
    }
    fn color4ubv(&mut self, rgba: [u8; 4]) {
        self.calls.push(format!("c4 {rgba:?}"));
    }
    fn tex_coord(&mut self, _: f32, _: f32) {}
    fn enable(&mut self, capability: u32) {
        self.calls.push(format!("en {capability:#x}"));
    }
    fn disable(&mut self, capability: u32) {
        self.calls.push(format!("dis {capability:#x}"));
    }
    fn bind_texture(&mut self, _: u32) {}
    fn push_projection(&mut self) {}
    fn pop_projection(&mut self) {}
    fn set_ortho_2d(&mut self) {}
    fn bind_modelview(&mut self, transform: &Transform) {
        self.calls.push(format!("mv {:?}", transform.translation));
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.calls.push(format!("rs {state} {value}"));
    }
    fn set_depth_write(&mut self, on: bool) {
        self.calls.push(format!("dw {on}"));
    }
}

impl DrawHost for TestHost {
    fn quads(&mut self) -> &mut crate::ui::quad_batch::QuadBatcher {
        &mut self.quads
    }
    fn rich_text_cx(&mut self) -> Option<RichTextCx<'_>> {
        Some(RichTextCx {
            viewport: [0, 0, 800, 600],
            projection: MIRROR,
            pipeline: &mut self.pipeline,
            formats: &self.formats,
            tiles: &mut self.tiles,
        })
    }
    fn immediate(&mut self) -> Option<&mut dyn crate::ui::immediate::Immediate> {
        Some(self)
    }
    fn submit_mesh(&mut self, _: &dyn crate::ui::control::ControlMesh, pass: MeshPass) {
        self.meshes.push(pass);
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
}

/// A rich-text window of the toolkit at [`placed`] with the tests' fonts.
fn toolkit_window() -> (Ui, NodeId) {
    let mut ui = Ui::new(Surface::new(800, 600));
    ui.fonts = Some(fonts());
    let id = ui
        .create_by_class(Some(b"Text"), b"RichTextWin", None, &mut Payload::None)
        .unwrap();
    ui.set_transform(id, &placed());
    if let Some(n) = ui.node_mut(id) {
        n.flags |= VISIBLE;
    }
    (ui, id)
}

fn draw(ui: &mut Ui, id: NodeId, host: &mut TestHost) {
    ui.with_widget(id, (), |w, ui| w.draw(ui, id, host));
}

fn window(ui: &mut Ui, id: NodeId) -> &mut RichTextWin {
    ui.widget_mut::<RichTextWin>(id).unwrap()
}

#[test]
fn layout_keeps_the_end_in_view_and_makes_the_bar() {
    let (mut ui, id) = toolkit_window();
    let mut host = TestHost::new();
    let win = window(&mut ui, id);
    win.base.set_auto_scrollbar(true);
    win.base.keep_scrolled_to_end = true;
    for line in 0..12 {
        win.append_node(&RdlNode::Text(units(&format!("line {line}"))));
        win.append_node(&RdlNode::Paragraph);
    }
    draw(&mut ui, id, &mut host);
    let b = &ui.widget::<RichTextWin>(id).unwrap().base;
    let (layout_height, pixel_height) = (b.layout_height, b.pixel_height);
    assert!(layout_height > pixel_height);
    // The bar is a UIScroll child named after the manager's serial, beside
    // the text, with the overflowing range and a page of the pixel height.
    let bar = b.scroll_bar_v.unwrap();
    assert_eq!(ui.parent(bar), Some(id));
    assert_eq!(ui.node(bar).unwrap().name(), b"ScrollV_1");
    assert_eq!(ui.scroll_v_serial, 2);
    let scroll = ui.widget::<UiScroll>(bar).unwrap();
    assert_eq!(scroll.range_max, layout_height as f32);
    assert_eq!(scroll.page, 90.0);
    assert_eq!(scroll.step, 9.0);
    assert_eq!((scroll.width, scroll.height), (0.02, 0.3));
    let gap_and_width = (0.002_f64 + 0.4_f64) as f32;
    assert_eq!(
        ui.node(bar).unwrap().position.x,
        (f64::from(gap_and_width) - 0.5) as f32
    );
    // More than 60 pixels from the end: the view stays at the top.
    let overflow = layout_height - pixel_height;
    assert!(overflow > 60);
    assert_eq!(ui.widget::<RichTextWin>(id).unwrap().base.scroll_y, 0);
    // The bar tells the window of its value (slot 23): the vertical scroll
    // follows it, truncated, and the bar takes the scroll back.
    let value = (overflow - 10) as f32 + 0.9;
    ui.widget_mut::<UiScroll>(bar).unwrap().set_value(value);
    assert!(ui.send_child_message(id, &ChildMessage::ScrollChanged { from: bar, value }));
    assert_eq!(
        ui.widget::<RichTextWin>(id).unwrap().base.scroll_y,
        overflow - 10
    );
    assert_eq!(
        ui.widget::<UiScroll>(bar).unwrap().value,
        (overflow - 10) as f32
    );
    // Within 60 pixels of it, a new line keeps the end in view.
    window(&mut ui, id).append_node(&RdlNode::Text(units("one more")));
    draw(&mut ui, id, &mut host);
    let b = &ui.widget::<RichTextWin>(id).unwrap().base;
    assert_eq!(b.scroll_y, b.layout_height - b.pixel_height);
    assert!(b.scroll_y > overflow);
    assert_eq!(b.trc.state.origin_v, -b.scroll_y);
    let scroll_y = b.scroll_y;
    assert_eq!(ui.widget::<UiScroll>(bar).unwrap().value, scroll_y as f32);
    // The text fits again: the automatic bar is deleted.
    let win = window(&mut ui, id);
    win.clear_content();
    win.append_node(&RdlNode::Text(units("short")));
    draw(&mut ui, id, &mut host);
    assert_eq!(
        ui.widget::<RichTextWin>(id).unwrap().base.scroll_bar_v,
        None
    );
    assert!(!ui.contains(bar));
}

/// A bar's own change (an arrow press) reaches the window while the bar is
/// busy: the window's value comes back to the bar before the bar returns.
#[test]
fn the_bars_own_change_scrolls_the_window() {
    let (mut ui, id) = toolkit_window();
    let mut host = TestHost::new();
    let win = window(&mut ui, id);
    for line in 0..12 {
        win.append_node(&RdlNode::Text(units(&format!("line {line}"))));
        win.append_node(&RdlNode::Paragraph);
    }
    draw(&mut ui, id, &mut host);
    ui.with_widget(id, (), |w, ui| {
        let win = w.as_any_mut().downcast_mut::<RichTextWin>().unwrap();
        win.base.show_scroll_bar_v(ui, id, true);
    });
    let bar = ui
        .widget::<RichTextWin>(id)
        .unwrap()
        .base
        .scroll_bar_v
        .unwrap();
    assert_eq!(ui.node(bar).unwrap().name(), b"ScrollV_1");
    {
        let scroll = ui.widget_mut::<UiScroll>(bar).unwrap();
        // Made after the layout: the range and page of the laid-out text.
        assert_eq!(scroll.page, 90.0);
        scroll.set_step(12.5);
    }
    // The bar's value moves by its step and the window follows.
    let pressed = ui.with_widget(bar, None, |w, ui| {
        let scroll = w.as_any_mut().downcast_mut::<UiScroll>().unwrap();
        // A held bottom arrow past its delay steps once (the draw's
        // auto-repeat).
        scroll.drag_part = 1;
        scroll.repeat_timer = -1.0;
        scroll.scroll_repeat(ui, bar, 0.0);
        Some(scroll.value)
    });
    assert_eq!(pressed, Some(12.0));
    assert_eq!(ui.widget::<RichTextWin>(id).unwrap().base.scroll_y, 12);
    // Hidden again: the bar is deleted.
    ui.with_widget(id, (), |w, ui| {
        let win = w.as_any_mut().downcast_mut::<RichTextWin>().unwrap();
        win.base.show_scroll_bar_v(ui, id, false);
    });
    assert!(!ui.contains(bar));
}

/// The draw submits the window's mesh before the image and outlines the
/// window in its frame colour after it.
#[test]
fn the_draw_submits_the_mesh_and_outlines_the_window() {
    let (mut ui, id) = toolkit_window();
    let mut host = TestHost::new();
    let win = window(&mut ui, id);
    win.append_node(&RdlNode::Text(units("framed")));
    win.base.draw_frame = true;
    win.base.frame_color = [1, 2, 3, 4];
    draw(&mut ui, id, &mut host);
    assert!(host.meshes.is_empty(), "no mesh, no submit");
    assert!(!host.tiles.is_empty());
    assert_eq!(
        host.calls,
        [
            "dis 0xb71",
            "dis 0xb44",
            "rs 7 0",
            "rs 14 0",
            "rs 22 1",
            "mv Vector { x: -0.5, y: 0.5, z: 0.0 }",
            "c4 [1, 2, 3, 4]",
            "dis 0xde1",
            "begin 7",
            "v 0 0 0",
            "v 0.4 0 0",
            "v 0.4 -0.3 0",
            "v 0 -0.3 0",
            "end",
            "en 0xb71",
            "dw true",
        ]
    );
}
