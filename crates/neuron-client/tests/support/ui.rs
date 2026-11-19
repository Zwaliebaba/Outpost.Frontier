//! The UI suites' line formats (`CppSource/Oracle/oracle_ui.cpp` writes the
//! same):
//!
//! ```text
//! {"case":"<n>","ok":"<0 or 1>","aspect":"<bits>","count":"<entries read>"}
//! {"case":"<n>","i":"<i>","type":"<hex>","name":"<hex>","rect":"<4 bits>","payload":"<payload>"}
//! ```
//!
//! Floats are `f32` bits in hex, caption units four hex digits each; the
//! payloads are `text:`, `button:`, `bounds:`, `object:`, `table:` and
//! `scroll:` with their fields separated by commas. The batcher suite's
//! lines are the recorder's calls (`oracle_ui_quads.cpp` lists them).

use std::collections::BTreeMap;
use std::fmt::Write as _;

use neuron_client::text::font::quad::{ScreenQuadRecord, TlVertex, clip_1d};
use neuron_client::ui::page::{EntryPayload, PageDesc, Style};
use neuron_client::ui::quad_batch::{Frame, QuadBatcher, QuadDevice, vertex_bytes};

use super::{hex, unhex};

fn bits(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

fn units(text: &[u16]) -> String {
    text.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

fn payload(p: &EntryPayload) -> String {
    match p {
        EntryPayload::Text {
            right,
            bottom,
            caption,
        } => format!("text:{},{},{}", bits(*right), bits(*bottom), units(caption)),
        EntryPayload::TextMsgButton {
            right,
            bottom,
            caption,
            extra_int,
            extra_id,
            style,
        } => {
            let style = match style {
                Style::None => "none".to_owned(),
                Style::Default => "default".to_owned(),
                Style::Named(name) => hex(name),
            };
            format!(
                "button:{},{},{},{extra_int},{extra_id},{style}",
                bits(*right),
                bits(*bottom),
                units(caption)
            )
        }
        EntryPayload::Bounds { right, bottom } => {
            format!("bounds:{},{}", bits(*right), bits(*bottom))
        }
        EntryPayload::WinObject {
            right,
            bottom,
            extra_name,
            extra_float,
        } => format!(
            "object:{},{},{},{}",
            bits(*right),
            bits(*bottom),
            hex(extra_name),
            bits(*extra_float)
        ),
        EntryPayload::Table {
            right,
            bottom,
            int5,
            int6,
        } => format!("table:{},{},{int5},{int6}", bits(*right), bits(*bottom)),
        EntryPayload::ScrollCycle {
            right,
            bottom,
            extra,
            flag,
        } => format!(
            "scroll:{},{},{},{},{},{},{}",
            bits(*right),
            bits(*bottom),
            bits(extra[0]),
            bits(extra[1]),
            bits(extra[2]),
            bits(extra[3]),
            u8::from(*flag)
        ),
    }
}

/// The lines of one case of `ui_page_desc.list`.
pub fn page_desc_lines(case: usize, line: &str) -> Vec<String> {
    let bytes = if line == "-" { Vec::new() } else { unhex(line) };
    let desc = PageDesc::load(&bytes);
    let mut out = vec![format!(
        "{{\"case\":\"{case}\",\"ok\":\"{}\",\"aspect\":\"{}\",\"count\":\"{}\"}}",
        u8::from(desc.ok),
        bits(desc.aspect),
        desc.entries.len()
    )];
    for (i, e) in desc.data_entries().iter().enumerate() {
        out.push(format!(
            "{{\"case\":\"{case}\",\"i\":\"{i}\",\"type\":\"{}\",\"name\":\"{}\",\"rect\":\"{},{},{},{}\",\"payload\":\"{}\"}}",
            hex(&e.type_name),
            hex(&e.name),
            bits(e.left),
            bits(e.top),
            bits(e.right),
            bits(e.bottom),
            payload(&e.payload)
        ));
    }
    out
}

/// A [`QuadDevice`] that writes the calls as `oracle_ui_quads.cpp`'s
/// recorder does: the render states read back what was set (0 before),
/// the locks answer base vertex 0 and offset 0.
struct QuadRecorder {
    case: usize,
    states: BTreeMap<u32, u32>,
    lines: Vec<String>,
}

impl QuadRecorder {
    fn event(&mut self, fields: &str) {
        self.lines
            .push(format!("{{\"case\":\"{}\",{fields}}}", self.case));
    }
}

impl QuadDevice for QuadRecorder {
    fn render_state(&mut self, state: u32) -> u32 {
        let v = self.states.get(&state).copied().unwrap_or(0);
        self.event(&format!(
            "\"ev\":\"get\",\"s\":\"{state}\",\"v\":\"{v:08x}\""
        ));
        v
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.states.insert(state, value);
        self.event(&format!(
            "\"ev\":\"rs\",\"s\":\"{state}\",\"v\":\"{value:08x}\""
        ));
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.event(&format!(
            "\"ev\":\"tss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"v\":\"{value:08x}\""
        ));
    }
    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.event(&format!("\"ev\":\"ps\",\"id\":\"{id}\",\"v\":\"{value}\""));
    }
    fn set_stage_texture(&mut self, texture: u32) {
        self.event(&format!(
            "\"ev\":\"tex\",\"st\":\"0\",\"h\":\"{texture:08x}\""
        ));
    }
    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        let n = vertices.len();
        self.event(&format!("\"ev\":\"vlock\",\"fvf\":\"1c4\",\"n\":\"{n}\""));
        let bytes: Vec<u8> = vertices.iter().flat_map(vertex_bytes).collect();
        self.event(&format!("\"ev\":\"verts\",\"data\":\"{}\"", hex(&bytes)));
        self.event(&format!("\"ev\":\"ilock\",\"n\":\"{}\"", indices.len()));
        let bytes: Vec<u8> = indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        self.event(&format!("\"ev\":\"idx\",\"data\":\"{}\"", hex(&bytes)));
        self.event("\"ev\":\"select\",\"base\":\"0\"");
        self.event(&format!(
            "\"ev\":\"draw\",\"prim\":\"4\",\"min\":\"0\",\"n\":\"{n}\",\"start\":\"0\",\"count\":\"{}\"",
            indices.len()
        ));
    }
}

fn h32(field: &str) -> u32 {
    u32::from_str_radix(field, 16).unwrap()
}

fn f32h(field: &str) -> f32 {
    f32::from_bits(h32(field))
}

/// The lines of one case of `ui_quad_batch.list`.
pub fn quad_batch_lines(case: usize, line: &str) -> Vec<String> {
    let mut batcher = QuadBatcher::new();
    let mut device = QuadRecorder {
        case,
        states: BTreeMap::new(),
        lines: Vec::new(),
    };
    for op in line.split(" ; ") {
        let f: Vec<&str> = op.split(' ').collect();
        match f[0] {
            "key" => batcher.set_material_key(h32(f[1])),
            "frame" => batcher.begin_frame(
                Frame {
                    texture: h32(f[1]),
                    u0: f32h(f[2]),
                    v0: f32h(f[3]),
                    u1: f32h(f[4]),
                    v1: f32h(f[5]),
                },
                f[6].parse().unwrap(),
                f[7] != "0",
            ),
            "rec" => batcher.append_record(ScreenQuadRecord {
                x0: f32h(f[1]),
                y0: f32h(f[2]),
                x1: f32h(f[3]),
                y1: f32h(f[4]),
                depth_a: f32h(f[5]),
                depth_b: f32h(f[6]),
                u0: f32h(f[7]),
                v0: f32h(f[8]),
                u1: f32h(f[9]),
                v1: f32h(f[10]),
                color: h32(f[11]),
            }),
            "flush" => batcher.flush(&mut device, h32(f[1])),
            "reset" => batcher.reset_draw_batch_cache(),
            "menu" => batcher.apply_screen_quad_render_states(&mut device),
            "invalidate" => batcher.state_cache.invalidate(),
            "get" => {
                device.states.insert(f[1].parse().unwrap(), h32(f[2]));
            }
            "clip" => {
                let (mut e0, mut e1, mut a0, mut a1) =
                    (f32h(f[1]), f32h(f[2]), f32h(f[3]), f32h(f[4]));
                let r = clip_1d(
                    &mut e0,
                    &mut e1,
                    &mut a0,
                    &mut a1,
                    f32h(f[5]),
                    f32h(f[6]),
                    f32h(f[7]),
                );
                device.event(&format!(
                    "\"op\":\"clip\",\"r\":\"{r}\",\"e0\":\"{}\",\"e1\":\"{}\",\"a0\":\"{}\",\"a1\":\"{}\"",
                    bits(e0),
                    bits(e1),
                    bits(a0),
                    bits(a1)
                ));
            }
            other => panic!("unknown operation {other}"),
        }
    }
    device.lines
}

// ---- the controls suite (`oracle_ui_controls.cpp`) ---------------------------------------

mod controls {
    use std::any::Any;
    use std::cell::RefCell;
    use std::rc::Rc;

    use neuron_client::ui::control::{ControlMesh, MeshLoader, WinControl, base};
    use neuron_client::ui::controls::list_box::{ListMetrics, ScreenBox, UiListBox};
    use neuron_client::ui::controls::scroll::{HitRect, ScrollShapes, UiScroll};
    use neuron_client::ui::controls::win_object::UiWinObject;
    use neuron_client::ui::immediate::Immediate;
    use neuron_client::ui::message::{ButtonAction, MouseButton};
    use neuron_client::ui::page::EntryPayload;
    use neuron_client::ui::quad_batch::QuadBatcher;
    use neuron_client::ui::shape::{ShapeTextures, TexShape};
    use neuron_client::ui::tree::{
        ChildMessage, DrawHost, MeshPass, NodeId, Payload, Property, Ui, Widget,
    };
    use neuron_client::ui::widescreen::Surface;
    use neuron_core::math::{Matrix, Transform, Vector};

    use super::{bits, hex, unhex, units};

    struct Log {
        case: usize,
        lines: Vec<String>,
    }

    type Shared = Rc<RefCell<Log>>;

    fn ev(log: &Shared, fields: &str) {
        let mut l = log.borrow_mut();
        let line = format!("{{\"case\":\"{}\",{fields}}}", l.case);
        l.lines.push(line);
    }

    fn bits3(v: Vector) -> String {
        format!("{} {} {}", bits(v.x), bits(v.y), bits(v.z))
    }

    fn bits_n(words: &[f32]) -> String {
        words.iter().map(|&w| bits(w)).collect::<Vec<_>>().join(" ")
    }

    fn rgba(c: [u8; 4]) -> String {
        format!("{:02x}{:02x}{:02x}{:02x}", c[0], c[1], c[2], c[3])
    }

    /// The immediate-mode stand-ins of the suite.
    struct RecImmediate(Shared);

    impl Immediate for RecImmediate {
        fn begin(&mut self, primitive: u32) {
            ev(&self.0, &format!("\"ev\":\"begin\",\"p\":\"{primitive}\""));
        }
        fn end(&mut self) {
            ev(&self.0, "\"ev\":\"end\"");
        }
        fn vertex(&mut self, x: f32, y: f32, z: f32) {
            ev(
                &self.0,
                &format!(
                    "\"ev\":\"v\",\"x\":\"{}\",\"y\":\"{}\",\"z\":\"{}\"",
                    bits(x),
                    bits(y),
                    bits(z)
                ),
            );
        }
        fn color3ub(&mut self, r: u8, g: u8, b: u8) {
            ev(
                &self.0,
                &format!("\"ev\":\"c3\",\"c\":\"{r:02x}{g:02x}{b:02x}\""),
            );
        }
        fn color4ubv(&mut self, c: [u8; 4]) {
            ev(&self.0, &format!("\"ev\":\"c4\",\"c\":\"{}\"", rgba(c)));
        }
        fn tex_coord(&mut self, u: f32, v: f32) {
            ev(
                &self.0,
                &format!("\"ev\":\"tc\",\"u\":\"{}\",\"v\":\"{}\"", bits(u), bits(v)),
            );
        }
        fn enable(&mut self, capability: u32) {
            ev(
                &self.0,
                &format!("\"ev\":\"en\",\"cap\":\"{capability:x}\""),
            );
        }
        fn disable(&mut self, capability: u32) {
            ev(
                &self.0,
                &format!("\"ev\":\"dis\",\"cap\":\"{capability:x}\""),
            );
        }
        fn bind_texture(&mut self, handle: u32) {
            ev(&self.0, &format!("\"ev\":\"tex\",\"h\":\"{handle:x}\""));
        }
        fn push_projection(&mut self) {
            ev(&self.0, "\"ev\":\"push\"");
        }
        fn pop_projection(&mut self) {
            ev(&self.0, "\"ev\":\"pop\"");
        }
        fn set_ortho_2d(&mut self) {
            ev(&self.0, "\"ev\":\"ortho\"");
        }
        fn bind_modelview(&mut self, t: &Transform) {
            let mut words = t.rotation.to_words().to_vec();
            words.extend_from_slice(&t.translation.to_words());
            ev(
                &self.0,
                &format!("\"ev\":\"mv\",\"m\":\"{}\"", bits_n(&words)),
            );
        }
        fn set_render_state(&mut self, state: u32, value: u32) {
            ev(
                &self.0,
                &format!(
                    "\"ev\":\"rs\",\"s\":\"{state}\",\"v\":\"{}\"",
                    value.cast_signed()
                ),
            );
        }
    }

    /// The texture library of the shapes: a record's handle is the record.
    struct Handles;

    impl ShapeTextures for Handles {
        fn find(&mut self, _: &[u8]) -> Option<u32> {
            None
        }
        fn add_ref(&mut self, _: u32) {}
        fn release(&mut self, _: u32) {}
        fn load_group(&mut self, _: &[u8]) -> Option<i32> {
            None
        }
        fn garbage_collect(&mut self, _: i32) {}
        fn generic_slot_4(&mut self) -> u32 {
            0
        }
        fn set_generic_slot_4(&mut self, _: u32) {}
        fn frame_handle(&mut self, record: u32, _: i32) -> u32 {
            record
        }
        fn texture_dim(&mut self, _: u32) -> (u32, u32) {
            (0, 0)
        }
    }

    struct Host {
        quads: QuadBatcher,
        imm: RecImmediate,
        textures: Handles,
        option: i32,
        log: Shared,
    }

    impl DrawHost for Host {
        fn quads(&mut self) -> &mut QuadBatcher {
            &mut self.quads
        }
        fn immediate(&mut self) -> Option<&mut dyn Immediate> {
            Some(&mut self.imm)
        }
        fn shape_textures(&mut self) -> Option<&mut dyn ShapeTextures> {
            Some(&mut self.textures)
        }
        fn scroll_bars_enabled(&self) -> bool {
            self.option != 0
        }
        fn submit_mesh(&mut self, _mesh: &dyn ControlMesh, pass: MeshPass) {
            match pass {
                MeshPass::Plain => ev(&self.log, "\"ev\":\"submit\""),
                MeshPass::Tinted { tint, lighting } => ev(
                    &self.log,
                    &format!(
                        "\"ev\":\"tinted\",\"tint\":\"{}\",\"l\":\"{}\"",
                        rgba(tint),
                        u8::from(lighting)
                    ),
                ),
            }
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
    }

    struct RecMesh(Shared);

    impl ControlMesh for RecMesh {
        fn transform(&self) -> Transform {
            Transform::IDENTITY
        }
        fn set_position(&mut self, p: Vector) {
            ev(
                &self.0,
                &format!(
                    "\"ev\":\"mpos\",\"x\":\"{}\",\"y\":\"{}\",\"z\":\"{}\"",
                    bits(p.x),
                    bits(p.y),
                    bits(p.z)
                ),
            );
            ev(&self.0, "\"ev\":\"mupd\"");
        }
        fn set_orientation(&mut self, _: Matrix) {
            ev(&self.0, "\"ev\":\"mori\"");
        }
        fn set_transform(&mut self, _: Transform) {
            ev(&self.0, "\"ev\":\"mxf\"");
        }
        fn hardpoint(&self, _: &[u8]) -> Option<Transform> {
            None
        }
    }

    struct RecLoader(Shared);

    impl MeshLoader for RecLoader {
        fn load(&mut self, path: &[u8], material: &[u8]) -> Option<Box<dyn ControlMesh>> {
            ev(
                &self.0,
                &format!(
                    "\"ev\":\"load\",\"path\":\"{}\",\"mat\":\"{}\"",
                    hex(path),
                    hex(material)
                ),
            );
            Some(Box::new(RecMesh(self.0.clone())))
        }
    }

    /// The recording parent.
    struct RecParent {
        control: WinControl,
        log: Shared,
        child: Rc<RefCell<Option<NodeId>>>,
    }

    impl RecParent {
        fn arg(&self, id: NodeId) -> String {
            if *self.child.borrow() == Some(id) {
                "self".to_owned()
            } else {
                "other".to_owned()
            }
        }
        fn record(&self, kind: &str, m: u32, a: &str, b: i32) {
            ev(
                &self.log,
                &format!("\"ev\":\"{kind}\",\"m\":\"{m:x}\",\"a\":\"{a}\",\"b\":\"{b}\""),
            );
        }
    }

    impl Widget for RecParent {
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
        fn win_control(&self) -> Option<&WinControl> {
            Some(&self.control)
        }
        fn win_control_mut(&mut self) -> Option<&mut WinControl> {
            Some(&mut self.control)
        }
        fn child_message(&mut self, _: &mut Ui, _: NodeId, message: &ChildMessage) -> bool {
            match message {
                ChildMessage::ListPick {
                    double_click,
                    text,
                    index,
                    ..
                } => {
                    let a = text.as_deref().map_or_else(|| "null".to_owned(), units);
                    let m = if *double_click { 0xF002 } else { 0xF001 };
                    self.record("msg", m, &a, *index);
                }
                ChildMessage::ScrollChanged { from } => {
                    let a = self.arg(*from);
                    self.record("msg", 0xF005, &a, 0);
                }
            }
            true
        }
        fn property(&mut self, _: &mut Ui, _: NodeId, property: Property<'_>) -> isize {
            match property {
                Property::ChildHit(id) => self.record("prop", 0x25, &self.arg(id), 0),
                Property::ChildToolTip(id) => self.record("prop", 0x4F, &self.arg(id), 0),
                Property::ChildButtonUp(id) => self.record("prop", 0x29, &self.arg(id), 0),
                Property::ScrollChanged(id, v) => self.record("prop", 0x3E, &self.arg(id), v),
                other => panic!("unexpected property {other:?}"),
            }
            0
        }
        fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
            base::destroy(ui, id, &mut self.control);
        }
    }

    /// The suite's font: 16-pixel lines, 7-pixel characters.
    struct Metrics;

    impl ListMetrics for Metrics {
        fn line_height(&mut self) -> i32 {
            16
        }
        fn text_width(&mut self, text: &[u16]) -> i32 {
            7 * i32::try_from(text.len()).unwrap()
        }
    }

    /// The suite's fixed projection of text objects.
    fn screen_box(position: Vector, width: f32, height: f32) -> ScreenBox {
        ScreenBox {
            x: (f64::from(position.x) * 400.0 + 320.0) as f32,
            y: (240.0 - f64::from(position.y) * 400.0) as f32,
            width: (f64::from(width) * 400.0) as f32,
            height: (f64::from(height) * 400.0) as f32,
        }
    }

    fn f(field: &str) -> f32 {
        super::f32h(field)
    }

    fn i(field: &str) -> i32 {
        field.parse().unwrap()
    }

    fn wide(field: &str) -> Vec<u16> {
        (0..field.len() / 4)
            .map(|k| u16::from_str_radix(&field[k * 4..k * 4 + 4], 16).unwrap())
            .collect()
    }

    fn button(f0: i32, f1: i32) -> MouseButton {
        MouseButton {
            index: (-1 - f0).cast_unsigned(),
            action: match f1 {
                1 => ButtonAction::Up,
                2 => ButtonAction::Down,
                _ => ButtonAction::DoubleClick,
            },
        }
    }

    fn rect_text(r: &HitRect) -> String {
        format!(
            "{} {} {} {} {}",
            bits(r.x0),
            bits(r.y0),
            bits(r.x1),
            bits(r.y1),
            u8::from(r.hot)
        )
    }

    fn scroll_state(ui: &Ui, id: NodeId, s: &UiScroll) -> String {
        let n = ui.node(id).unwrap();
        let rects = [
            &s.top_arrow,
            &s.bottom_arrow,
            &s.track_before,
            &s.track_after,
            &s.thumb,
        ]
        .map(rect_text)
        .join(" ");
        let mut inv = s.inverse_layout.rotation.to_words().to_vec();
        inv.extend_from_slice(&s.inverse_layout.translation.to_words());
        format!(
            "\"ev\":\"scroll\",\"flags\":\"{:02x}\",\"pos\":\"{}\",\"v\":\"{}\",\"rect\":\"{rects}\",\"arrow\":\"{}\",\"drag\":\"{}\",\"rt\":\"{}\",\"dragging\":\"{}\",\"doff\":\"{}\",\"inv\":\"{}\",\"colors\":\"{:08x} {:08x} {:08x} {:08x}\",\"soff\":\"{}\",\"dpage\":\"{}\"",
            n.flags,
            bits3(n.position),
            bits_n(&[s.value, s.range_min, s.range_max, s.page, s.step]),
            bits(s.arrow_size),
            s.drag_part,
            bits(s.repeat_timer),
            u8::from(s.dragging),
            bits(s.drag_offset),
            bits_n(&inv),
            s.arrow_idle_color,
            s.arrow_hot_color,
            s.bar_idle_color,
            s.bar_hot_color,
            bits3(s.scroll_offset),
            bits(s.default_page),
        )
    }

    fn with<W: Widget, R>(ui: &mut Ui, id: NodeId, f: impl FnOnce(&mut W, &mut Ui) -> R) -> R {
        let mut out = None;
        ui.with_widget(id, (), |w, ui| {
            out = Some(f(w.as_any_mut().downcast_mut::<W>().unwrap(), ui));
        });
        out.unwrap()
    }

    /// The list box's update as `ListBox::Update` runs it, over the suite's
    /// projection and font.
    fn list_update(ui: &mut Ui, id: NodeId) {
        with::<UiListBox, _>(ui, id, |w, ui| {
            ui.base_update(id);
            w.list.advance_timers(ui.frame_time);
            let position = ui.node(id).unwrap().position;
            let screen = screen_box(position, w.list.width, w.list.height);
            w.list.update_hover(screen, ui.cursor, &mut Metrics);
        });
    }

    /// The list box's draw as `ListBox::Draw` runs it: the rows' colours
    /// and paints, then the frame.
    fn list_draw(ui: &mut Ui, id: NodeId, host: &mut Host) {
        with::<UiListBox, _>(ui, id, |w, ui| {
            host.imm.disable(0xB71);
            host.imm.disable(0xDE1);
            let position = ui.node(id).unwrap().position;
            let screen = screen_box(position, w.list.width, w.list.height);
            for row in w.list.row_paints(screen, &ui.pulse, &mut Metrics) {
                ev(
                    &host.log,
                    &format!("\"ev\":\"color\",\"c\":\"{:08x}\"", row.color),
                );
                if let Some(trim) = row.trim_right {
                    let text = w.list.item_text(row.index).unwrap_or_default();
                    ev(
                        &host.log,
                        &format!(
                            "\"ev\":\"paint\",\"t\":\"{}\",\"x\":\"{}\",\"y\":\"{}\",\"l\":\"{}\",\"r\":\"{}\"",
                            units(text),
                            bits(row.x),
                            bits(row.y),
                            bits(0.0),
                            bits(trim)
                        ),
                    );
                }
            }
            w.list.draw_frame(&mut host.imm, screen, &mut Metrics);
        });
    }

    fn result(log: &Shared, v: impl std::fmt::Display) {
        ev(log, &format!("\"ev\":\"r\",\"v\":\"{v}\""));
    }

    fn payload(t: &[&str]) -> EntryPayload {
        match t[1] {
            "UIListBox" => EntryPayload::Bounds {
                right: f(t[2]),
                bottom: f(t[3]),
            },
            "UIScroll" | "UICycle" => EntryPayload::ScrollCycle {
                right: f(t[2]),
                bottom: f(t[3]),
                extra: [f(t[4]), f(t[5]), f(t[6]), f(t[7])],
                flag: i(t[8]) != 0,
            },
            _ => EntryPayload::WinObject {
                right: f(t[2]),
                bottom: f(t[3]),
                extra_name: t.get(5).map_or_else(Vec::new, |p| unhex(p)),
                extra_float: f(t[4]),
            },
        }
    }

    /// One state line.
    fn state(ui: &Ui, id: NodeId, class: &str) -> String {
        match class {
            "UIListBox" => {
                let l = &ui.widget::<UiListBox>(id).unwrap().list;
                format!(
                    "\"ev\":\"list\",\"hover\":\"{}\",\"f80\":\"{}\",\"sel\":\"{}\",\"count\":\"{}\",\"first\":\"{}\",\"rows\":\"{}\",\"t\":\"{}\",\"w\":\"{}\",\"h\":\"{}\"",
                    l.hover_index,
                    l.field_80,
                    l.selected_index,
                    l.item_count,
                    l.first_visible,
                    l.visible_rows,
                    bits_n(&[l.timer_890, l.timer_894]),
                    bits(l.width),
                    bits(l.height)
                )
            }
            "UIScroll" | "UICycle" => scroll_state(ui, id, ui.widget::<UiScroll>(id).unwrap()),
            _ => {
                let n = ui.node(id).unwrap();
                let o = ui.widget::<UiWinObject>(id).unwrap();
                format!(
                    "\"ev\":\"obj\",\"flags\":\"{:02x}\",\"pos\":\"{}\",\"orient\":\"{}\",\"origin\":\"{}\",\"corner\":\"{}\"",
                    n.flags,
                    bits3(n.position),
                    bits_n(&n.orientation.to_words()),
                    bits3(o.origin),
                    bits3(o.corner)
                )
            }
        }
    }

    /// The operations of one control class.
    fn class_op(ui: &mut Ui, id: NodeId, log: &Shared, t: &[&str]) {
        let v3 = |k: usize| Vector::new(f(t[k]), f(t[k + 1]), f(t[k + 2]));
        match t[0] {
            "item" => {
                let r =
                    with::<UiListBox, _>(ui, id, |w, ui| w.list.add_item(ui, &wide(t[1]), i(t[2])));
                result(log, r);
            }
            "settext" => {
                let r = with::<UiListBox, _>(ui, id, |w, ui| {
                    w.list.set_item_text(ui, i(t[1]), &wide(t[2]))
                });
                result(log, u8::from(r));
            }
            "data" => result(
                log,
                ui.widget::<UiListBox>(id).unwrap().list.item_data(i(t[1])),
            ),
            "f80" | "frame" | "rules" | "rows" | "first" | "sel" => {
                let l = &mut ui.widget_mut::<UiListBox>(id).unwrap().list;
                let v = i(t[1]);
                match t[0] {
                    "f80" => l.field_80 = v,
                    "frame" => l.draw_frame = v != 0,
                    "rules" => l.draw_row_rules = v != 0,
                    "rows" => l.visible_rows = v,
                    "first" => l.first_visible = v,
                    _ => l.selectable = v != 0,
                }
            }
            "range" | "value" | "page" | "step" | "size" | "stepmode" | "horiz" | "custom"
            | "active" => {
                let s = ui.widget_mut::<UiScroll>(id).unwrap();
                match t[0] {
                    "range" => s.set_range(f(t[1]), f(t[2])),
                    "value" => s.set_value(f(t[1])),
                    "page" => s.set_page(f(t[1])),
                    "step" => s.set_step(f(t[1])),
                    "size" => s.set_size(f(t[1]), f(t[2])),
                    "stepmode" => s.step_mode = i(t[1]) != 0,
                    "horiz" => s.horizontal = i(t[1]) != 0,
                    "custom" => s.custom_color_on = i(t[1]) != 0,
                    _ => s.active = i(t[1]) != 0,
                }
            }
            "soff" => {
                let v = v3(1);
                with::<UiScroll, _>(ui, id, |w, ui| w.set_scroll_offset(ui, id, v));
            }
            "offset" => {
                let v = v3(1);
                let r = ui.with_widget(id, 0, |w, ui| w.property(ui, id, Property::Offset(v)));
                result(log, r);
            }
            "prop" => {
                let (a, b) = (i(t[2]), i(t[3]));
                let property = match t[1] {
                    "37" => Property::ScrollRange(a, b),
                    "38" => Property::ScrollValue(a),
                    _ => Property::GetScrollValue,
                };
                let r = ui.with_widget(id, 0, |w, ui| w.property(ui, id, property));
                result(log, r);
            }
            "flags" => {
                ui.widget_mut::<UiWinObject>(id).unwrap().object.draw_flags =
                    i(t[1]).to_le_bytes()[0];
            }
            "tint" => {
                let packed = u32::from_str_radix(t[1], 16).unwrap();
                ui.widget_mut::<UiWinObject>(id).unwrap().object.tint = packed.to_be_bytes();
            }
            other => panic!("unknown operation {other}"),
        }
    }

    pub fn lines(case: usize, line: &str) -> Vec<String> {
        let log: Shared = Rc::new(RefCell::new(Log {
            case,
            lines: Vec::new(),
        }));
        let mut ui = Ui::new(Surface::new(640, 480));
        ui.mesh_loader = Some(Box::new(RecLoader(log.clone())));
        let shape = |record| {
            Some(TexShape {
                texture: Some(record),
                ..TexShape::default()
            })
        };
        ui.ui_shapes = ScrollShapes {
            arrow: shape(0x101),
            top_bottom: shape(0x102),
            mid: shape(0x103),
        };
        ui.pulse.reset();
        let mut host = Host {
            quads: QuadBatcher::new(),
            imm: RecImmediate(log.clone()),
            textures: Handles,
            option: 1,
            log: log.clone(),
        };
        let child = Rc::new(RefCell::new(None));
        let parent = ui
            .create(
                Box::new(RecParent {
                    control: WinControl::new(),
                    log: log.clone(),
                    child: child.clone(),
                }),
                Some("WinControl"),
                None,
                &mut Payload::None,
            )
            .unwrap();
        let mut object: Option<NodeId> = None;
        let mut class = String::new();
        for op in line.split(" ; ") {
            let t: Vec<&str> = op.split(' ').collect();
            if t[0] == "new" {
                class = t[1].to_owned();
                let payload = payload(&t);
                object = ui.create_by_class(
                    Some(b"C"),
                    class.as_bytes(),
                    Some(parent),
                    &mut Payload::Page(&payload),
                );
                *child.borrow_mut() = object;
                ev(
                    &log,
                    &format!("\"ev\":\"new\",\"ok\":\"{}\"", u8::from(object.is_some())),
                );
                continue;
            }
            let Some(id) = object else { continue };
            let list = class == "UIListBox";
            let v3 = |k: usize| Vector::new(f(t[k]), f(t[k + 1]), f(t[k + 2]));
            match t[0] {
                "xform" => {
                    let words: Vec<f32> = (1..13).map(|k| f(t[k])).collect();
                    let rotation = Matrix::from_words(words[..9].try_into().unwrap());
                    let transform =
                        Transform::new(rotation, Vector::new(words[9], words[10], words[11]));
                    ui.set_transform(id, &transform);
                }
                "pos" => ui.set_position(id, v3(1)),
                "ray" => ui.set_mouse_ray(v3(1)),
                "cursor" => ui.cursor = (i(t[1]), i(t[2])),
                "dt" => ui.frame_time = f64::from(f(t[1])),
                "pulse" => ui.pulse.advance(f64::from(f(t[1]))),
                "opt" => host.option = i(t[1]),
                "update" if list => list_update(&mut ui, id),
                "update" => ui.with_widget(id, (), |w, ui| w.update(ui, id)),
                "draw" if list => list_draw(&mut ui, id, &mut host),
                "draw" => ui.with_widget(id, (), |w, ui| w.draw(ui, id, &mut host)),
                "over" => {
                    let ray = ui.mouse_ray();
                    ui.with_widget(id, (), |w, ui| w.mouse_over(ui, id, ray));
                }
                "leave" => {
                    let ray = ui.mouse_ray();
                    ui.with_widget(id, (), |w, ui| w.mouse_leave(ui, id, ray));
                }
                "btn" => {
                    let event = button(i(t[1]), i(t[2]));
                    let r = ui.with_widget(id, false, |w, ui| w.mouse_event(ui, id, event));
                    ev(&log, &format!("\"ev\":\"btn\",\"r\":\"{}\"", u8::from(r)));
                }
                "hit" => {
                    let ray = ui.mouse_ray();
                    let r = ui.with_widget(id, false, |w, ui| w.hit_test(ui, id, ray));
                    ev(&log, &format!("\"ev\":\"hit\",\"r\":\"{}\"", u8::from(r)));
                }
                "state" => {
                    let line = state(&ui, id, &class);
                    ev(&log, &line);
                }
                _ => class_op(&mut ui, id, &log, &t),
            }
        }
        drop(host);
        drop(ui);
        Rc::try_unwrap(log)
            .ok()
            .map(|l| l.into_inner().lines)
            .unwrap_or_else(|| panic!("the log is still shared"))
    }
}

/// The lines of one case of `ui_controls.list`.
pub fn controls_lines(case: usize, line: &str) -> Vec<String> {
    controls::lines(case, line)
}
