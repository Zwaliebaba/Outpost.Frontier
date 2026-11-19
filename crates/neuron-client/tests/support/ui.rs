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
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;

    use neuron_client::text::font::FontId;
    use neuron_client::text::font::string::DrawOrigin;
    use neuron_client::text::rdl::{RdlNode, RenderDisplayList, TextAttributes};
    use neuron_client::ui::control::{ControlMesh, MeshLoader, WinControl, base};
    use neuron_client::ui::controls::button::{
        ButtonControl, ButtonWidget, RichTextButtonWidget, ToggleButtonWidget,
    };
    use neuron_client::ui::controls::fast_table::{
        ScrollTableCallback, TableEdges, UiFastTable, UiScrollTable,
    };
    use neuron_client::ui::controls::list_box::{ListMetrics, ScreenBox, UiListBox};
    use neuron_client::ui::controls::rich_text_edit::{RichTextEditWin, UiTextEdit};
    use neuron_client::ui::controls::scroll::{HitRect, ScrollShapes, UiScroll};
    use neuron_client::ui::controls::scroll_control::{PartId, ScrollControlWidget};
    use neuron_client::ui::controls::static_control::StaticControlWidget;
    use neuron_client::ui::controls::table::UiTable;
    use neuron_client::ui::controls::text_control::{
        ControlText, EditControl, TextControl, TextControlWidget,
    };
    use neuron_client::ui::controls::text_win::TextProjection;
    use neuron_client::ui::controls::win_object::UiWinObject;
    use neuron_client::ui::controls::wire::{WireControl, WireWidget};
    use neuron_client::ui::immediate::Immediate;
    use neuron_client::ui::message::{ButtonAction, KeyMods, MouseButton};
    use neuron_client::ui::page::EntryPayload;
    use neuron_client::ui::quad_batch::QuadBatcher;
    use neuron_client::ui::services::{HoverKey, HudRecord, ToolTipHover, UiServices};
    use neuron_client::ui::shape::{ShapeTextures, TexShape};
    use neuron_client::ui::tree::{
        ChildMessage, DrawHost, MeshPass, NodeId, Payload, Property, Ui, Widget, WindowView,
    };
    use neuron_client::ui::widescreen::Surface;
    use neuron_core::math::{Matrix, Transform, Vector};

    use super::{bits, hex, unhex, units};

    struct Log {
        case: usize,
        lines: Vec<String>,
        /// The teardown after the case records nothing, as the oracle's.
        muted: bool,
    }

    type Shared = Rc<RefCell<Log>>;

    fn ev(log: &Shared, fields: &str) {
        let mut l = log.borrow_mut();
        if l.muted {
            return;
        }
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

    fn mesh_ev(name: &str, index: usize) -> String {
        if index > 0 {
            format!("\"ev\":\"{name}\",\"i\":\"{index}\"")
        } else {
            format!("\"ev\":\"{name}\"")
        }
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
        fn set_depth_write(&mut self, on: bool) {
            ev(
                &self.0,
                &format!("\"ev\":\"dw\",\"v\":\"{}\"", u8::from(on)),
            );
        }
        fn batch_line(&mut self, from: Vector, to: Vector, argb: u32, _rgb: u32, width: f32) {
            ev(
                &self.0,
                &format!(
                    "\"ev\":\"line\",\"f\":\"{}\",\"t\":\"{}\",\"c\":\"{argb:08x}\",\"w\":\"{}\"",
                    bits3(from),
                    bits3(to),
                    bits(width)
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
        options: [i32; 0x20],
        log: Shared,
    }

    /// The holder's tint floats of a body pass (`SetHolderTint`).
    fn tint_floats(rgba: [u8; 4]) -> [f32; 4] {
        let inv = f64::from(1.0_f32 / 255.0);
        rgba.map(|b| (f64::from(b) * inv) as f32)
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
        fn game_option(&self, index: u32) -> i32 {
            self.options.get(index as usize).copied().unwrap_or(1)
        }
        fn submit_mesh(&mut self, _mesh: &dyn ControlMesh, pass: MeshPass) {
            let log = &self.log;
            match pass {
                MeshPass::Plain => ev(log, "\"ev\":\"submit\""),
                MeshPass::Body {
                    tint,
                    front_cull,
                    scale,
                } => {
                    ev(log, "\"ev\":\"submit\"");
                    let t = tint_floats(tint);
                    if front_cull || scale != 1.0 || t.iter().any(|&v| v != 1.0) {
                        ev(
                            log,
                            &format!(
                                "\"ev\":\"body\",\"t\":\"{}\",\"cull\":\"{}\",\"s\":\"{}\"",
                                bits_n(&t),
                                u8::from(front_cull),
                                bits(scale)
                            ),
                        );
                    }
                }
                MeshPass::Tinted { tint, lighting } => ev(
                    log,
                    &format!(
                        "\"ev\":\"tinted\",\"tint\":\"{}\",\"l\":\"{}\"",
                        rgba(tint),
                        u8::from(lighting)
                    ),
                ),
                MeshPass::TintedScaled {
                    tint,
                    front_cull,
                    lighting,
                    scale,
                } => {
                    ev(
                        log,
                        &format!(
                            "\"ev\":\"tinted\",\"tint\":\"{}\",\"l\":\"{}\"",
                            rgba(tint),
                            u8::from(lighting)
                        ),
                    );
                    if !front_cull || scale != 1.0 {
                        ev(
                            log,
                            &format!(
                                "\"ev\":\"tscale\",\"cull\":\"{}\",\"s\":\"{}\"",
                                u8::from(front_cull),
                                bits(scale)
                            ),
                        );
                    }
                }
                MeshPass::Outlined { wire, scale } => ev(
                    log,
                    &format!(
                        "\"ev\":\"outlined\",\"w\":\"{}\",\"s\":\"{}\"",
                        rgba(wire),
                        bits(scale)
                    ),
                ),
                MeshPass::Direct { .. } => panic!("no case draws a z-clear pass"),
            }
        }
        /// The window camera's rectangle, planes and angle, as the C++
        /// stand-in of `Win3DApplyCameraPerspective` records them; the
        /// window's end as its `Win3DSetViewportRect`.
        fn window_view(&mut self, view: Option<WindowView>) {
            match view {
                Some(v) => ev(
                    &self.log,
                    &format!(
                        "\"ev\":\"winview\",\"vp\":\"{} {} {} {}\",\"near\":\"{}\",\"far\":\"{}\",\"fov\":\"{}\"",
                        v.rect[0],
                        v.rect[1],
                        v.rect[2],
                        v.rect[3],
                        bits(v.z_near),
                        bits(v.z_far),
                        bits(v.fov_y_deg)
                    ),
                ),
                None => ev(&self.log, "\"ev\":\"winview\",\"vp\":\"-\""),
            }
        }
        fn clear_engine_lights(&mut self) {
            ev(&self.log, "\"ev\":\"lclear\"");
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
    }

    /// An engine instance: the transform the calls set, recorded.
    struct RecMesh {
        log: Shared,
        index: usize,
        transform: Transform,
    }

    impl RecMesh {
        fn pos(&mut self, p: Vector) {
            self.transform.translation = p;
            ev(
                &self.log,
                &format!(
                    "{},\"x\":\"{}\",\"y\":\"{}\",\"z\":\"{}\"",
                    mesh_ev("mpos", self.index),
                    bits(p.x),
                    bits(p.y),
                    bits(p.z)
                ),
            );
        }
        fn ori(&mut self, m: Matrix) {
            self.transform.rotation = m;
            ev(
                &self.log,
                &format!(
                    "{},\"m\":\"{}\"",
                    mesh_ev("mori", self.index),
                    bits_n(&m.to_words())
                ),
            );
        }
        fn upd(&self) {
            ev(&self.log, &mesh_ev("mupd", self.index));
        }
    }

    impl ControlMesh for RecMesh {
        fn transform(&self) -> Transform {
            self.transform
        }
        fn set_position(&mut self, p: Vector) {
            self.pos(p);
            self.upd();
        }
        fn set_orientation(&mut self, m: Matrix) {
            self.ori(m);
            self.upd();
        }
        fn set_transform(&mut self, t: Transform) {
            self.transform = t;
            ev(
                &self.log,
                &format!(
                    "{},\"m\":\"{}\"",
                    mesh_ev("mxf", self.index),
                    bits_n(&t.to_words())
                ),
            );
            self.upd();
        }
        fn hardpoint(&self, _: &[u8]) -> Option<Transform> {
            None
        }
        fn radius(&self, _: u32) -> f32 {
            0.25
        }
        fn put_orientation(&mut self, m: Matrix) {
            self.ori(m);
        }
        fn put_position(&mut self, p: Vector) {
            self.pos(p);
        }
    }

    struct RecLoader(Shared, Rc<Cell<usize>>);

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
            let index = self.1.get().min(7);
            self.1.set(index + 1);
            Some(Box::new(RecMesh {
                log: self.0.clone(),
                index,
                transform: Transform::IDENTITY,
            }))
        }
    }

    /// The services' stand-ins: a UI sound's slot is its name's length.
    struct RecServices {
        log: Shared,
        elapsed: Rc<Cell<f64>>,
        /// The object under test: the HUD stand-ins record its calls.
        child: Rc<RefCell<Option<NodeId>>>,
        /// The input focus and the ones it replaced, as the HUD keeps them.
        focus: Option<NodeId>,
        focus_stack: Vec<NodeId>,
    }

    impl RecServices {
        fn is_child(&self, node: NodeId) -> bool {
            *self.child.borrow() == Some(node)
        }
    }

    impl UiServices for RecServices {
        fn find_ui_sound_slot(&mut self, name: &[u8]) -> i32 {
            let len = name.iter().position(|&b| b == 0).unwrap_or(name.len());
            if len == 0 { -1 } else { len as i32 }
        }
        fn play_or_rewind_ui_sound_slot(&mut self, slot: u32) {
            ev(&self.log, &format!("\"ev\":\"sound\",\"s\":\"{slot}\""));
        }
        fn play_ui_sound_slot_flag1(&mut self, slot: u32) {
            ev(&self.log, &format!("\"ev\":\"tplay\",\"s\":\"{slot}\""));
        }
        fn stop_ui_sound_slot(&mut self, slot: u32) {
            ev(&self.log, &format!("\"ev\":\"tstop\",\"s\":\"{slot}\""));
        }
        fn play_ui_rollover_sound(&mut self) {
            ev(&self.log, "\"ev\":\"roll\"");
        }
        fn post_command(&mut self, command: u32) {
            ev(&self.log, &format!("\"ev\":\"post\",\"c\":\"{command}\""));
        }
        fn elapsed_time(&mut self) -> f64 {
            self.elapsed.get()
        }
        fn mouse_set_cursor(&mut self, name: &[u8]) {
            let name = String::from_utf8_lossy(name).into_owned();
            ev(&self.log, &format!("\"ev\":\"mcursor\",\"n\":\"{name}\""));
        }
        fn input_focus(&mut self) -> Option<NodeId> {
            self.focus
        }
        fn push_input_focus(&mut self, node: NodeId) {
            if self.is_child(node) {
                ev(&self.log, "\"ev\":\"focus\",\"op\":\"push\",\"h\":\"self\"");
            }
            if let Some(current) = self.focus {
                self.focus_stack.push(current);
            }
            self.focus = Some(node);
        }
        fn pop_input_focus(&mut self, node: NodeId) {
            if self.is_child(node) {
                ev(&self.log, "\"ev\":\"focus\",\"op\":\"pop\",\"h\":\"self\"");
            }
            if self.focus == Some(node) {
                self.focus = self.focus_stack.pop();
                return;
            }
            if let Some(at) = self.focus_stack.iter().position(|&n| n == node) {
                self.focus_stack.remove(at);
            }
        }
        fn register_event(&mut self, node: NodeId, msg: u32) {
            if self.is_child(node) {
                ev(&self.log, &format!("\"ev\":\"evreg\",\"m\":\"{msg:x}\""));
            }
        }
        fn unregister_event(&mut self, node: NodeId, msg: u32) {
            if self.is_child(node) {
                ev(&self.log, &format!("\"ev\":\"evunreg\",\"m\":\"{msg:x}\""));
            }
        }
    }

    /// The `Font::String` stand-ins: the text kept, a line when it has a
    /// text or a number, 7 pixels a character by 16.
    struct RecText {
        log: Shared,
        text: Option<Vec<u16>>,
        number: Option<Vec<u8>>,
        font: Option<FontId>,
        slots: bool,
    }

    impl RecText {
        fn len(&self) -> usize {
            match &self.text {
                Some(t) if !t.is_empty() => t.len(),
                _ => self.number.as_ref().map_or(0, Vec::len),
            }
        }
        fn lay_out(&mut self) {
            self.slots = self.len() > 0 || self.number.is_some();
        }
        fn drop_text(&mut self) {
            self.text = None;
            self.slots = false;
            self.number = None;
        }
    }

    impl ControlText for RecText {
        fn set_scale(&mut self, _: &mut Ui, width: f32, depth: f32) {
            ev(
                &self.log,
                &format!(
                    "\"ev\":\"fscale\",\"w\":\"{}\",\"z\":\"{}\"",
                    bits(width),
                    bits(depth)
                ),
            );
        }
        fn set_text(&mut self, _: &mut Ui, text: Option<&[u16]>) {
            let cut = |t: &[u16]| t[..t.iter().position(|&u| u == 0).unwrap_or(t.len())].to_vec();
            let incoming = text.map(cut);
            if incoming.as_deref().unwrap_or(&[]) == self.text.as_deref().unwrap_or(&[]) {
                return;
            }
            if self.text.as_ref().is_some_and(|t| !t.is_empty()) {
                self.drop_text();
            }
            match incoming {
                Some(t) => self.text = Some(t),
                None => {
                    if let Some(t) = &mut self.text {
                        t.clear();
                    }
                }
            }
            let shown = self
                .text
                .as_deref()
                .map_or_else(|| "null".to_owned(), units);
            ev(&self.log, &format!("\"ev\":\"ftext\",\"t\":\"{shown}\""));
        }
        fn text(&self) -> Option<&[u16]> {
            self.text.as_deref()
        }
        fn set_number_text(&mut self, _: &mut Ui, format: &[u8]) {
            if self.text.as_ref().is_some_and(|t| !t.is_empty()) {
                self.drop_text();
            }
            self.number = Some(format.to_vec());
            ev(
                &self.log,
                &format!("\"ev\":\"fnum\",\"f\":\"{}\"", hex(format)),
            );
        }
        fn font(&self) -> Option<FontId> {
            self.font
        }
        fn set_font(&mut self, _: &mut Ui, font: Option<FontId>) {
            self.font = font;
        }
        fn find_font(&self, _: &Ui, _: &[u8]) -> Option<FontId> {
            Some(FontId(0))
        }
        fn has_child_slots(&self) -> bool {
            self.slots
        }
        fn child_count(&mut self, _: &mut Ui, _: &mut dyn DrawHost) -> usize {
            self.lay_out();
            usize::from(self.slots)
        }
        fn extent(&mut self, _: &mut Ui, _: Option<&mut dyn DrawHost>) -> (i32, i32) {
            self.lay_out();
            (7 * self.len() as i32, if self.slots { 16 } else { 0 })
        }
        fn draw_children(
            &mut self,
            _: &mut Ui,
            _: &mut dyn DrawHost,
            o: DrawOrigin,
            left: f32,
            right: f32,
        ) {
            self.lay_out();
            ev(
                &self.log,
                &format!(
                    "\"ev\":\"fdraw\",\"x\":\"{}\",\"y\":\"{}\",\"w\":\"{}\",\"h\":\"{}\",\"hm\":\"{}\",\"vm\":\"{}\",\"l\":\"{}\",\"r\":\"{}\"",
                    bits(o.x),
                    bits(o.y),
                    bits(o.width),
                    bits(o.height),
                    o.h_mode,
                    o.v_mode,
                    bits(left),
                    bits(right)
                ),
            );
        }
        fn clear_contents(&mut self, _: &mut Ui) {
            self.drop_text();
        }
        fn set_draw_color(&mut self, _: &mut Ui, argb: u32) {
            ev(&self.log, &format!("\"ev\":\"color\",\"c\":\"{argb:08x}\""));
        }
        fn set_multi_line(&mut self, _: bool) {}
        fn set_max_lines(&mut self, _: i32) {}
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
    }

    /// The suite's fixed projection of text objects.
    struct Fixed;

    impl TextProjection for Fixed {
        fn point(&self, world: Vector) -> (f32, f32) {
            (
                (f64::from(world.x) * 400.0 + 320.0) as f32,
                (240.0 - f64::from(world.y) * 400.0) as f32,
            )
        }
        fn extent(&self, width: f32, height: f32, _z: f32) -> (f32, f32) {
            (
                (f64::from(width) * 400.0) as f32,
                (f64::from(height) * 400.0) as f32,
            )
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
                ChildMessage::EditEnter { from } => self.record("msg", 0xF004, &self.arg(*from), 0),
                ChildMessage::EditEscape { from } => {
                    self.record("msg", 0xF003, &self.arg(*from), 0)
                }
                ChildMessage::TableSelect { from, row } => {
                    self.record("msg", 0x300D, &self.arg(*from), row.cast_signed());
                }
                ChildMessage::TableDoubleClick { from, row } => {
                    self.record("msg", 0xF002, &self.arg(*from), row.cast_signed());
                }
                ChildMessage::TableCellPress { from, cell } => {
                    self.record("msg", 0xF001, &self.arg(*from), cell.cast_signed());
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
                Property::FromChild { msg, from, value } => {
                    self.record("prop", msg.cast_unsigned(), &self.arg(from), value);
                }
                Property::EditText { text, enter, .. } => {
                    let a = text.as_deref().map_or_else(|| "null".to_owned(), units);
                    self.record("prop", 0x45, &a, i32::from(enter));
                }
                other => panic!("unexpected property {other:?}"),
            }
            0
        }
        fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
            base::destroy(ui, id, &mut self.control);
        }
        fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
            base::set_position(ui, id, &mut self.control, position);
        }
        fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
            base::set_orientation(ui, id, &mut self.control, orientation);
        }
    }

    /// The tooltip's stand-in: the hover records and hides fed to it.
    struct RecTip {
        log: Shared,
        child: Rc<RefCell<Option<NodeId>>>,
    }

    impl Widget for RecTip {
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
        fn tooltip_hover(&mut self, _: &mut Ui, _: NodeId, h: &ToolTipHover) {
            let key = match h.key {
                HoverKey::Ids(v) => v.to_string(),
                HoverKey::Object(id) if *self.child.borrow() == Some(id) => "self".to_owned(),
                HoverKey::Object(_) => "other".to_owned(),
                HoverKey::Part(id, n) if *self.child.borrow() == Some(id) => {
                    format!("self+{}", n + 1)
                }
                HoverKey::Part(..) => "other".to_owned(),
            };
            ev(
                &self.log,
                &format!(
                    "\"ev\":\"hover\",\"k\":\"{key}\",\"a\":\"{}\",\"x\":\"{}\",\"p\":\"{}\",\"e\":\"{}\",\"l\":\"{}\"",
                    h.ids_alt,
                    h.aux,
                    bits3(h.world_pos),
                    bits3(h.half_extents),
                    u8::from(h.live)
                ),
            );
        }
        fn tooltip_hide(&mut self, _: &mut Ui, _: NodeId) {
            ev(&self.log, "\"ev\":\"hidetip\"");
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
        let (x, y) = Fixed.point(position);
        let (w, h) = Fixed.extent(width, height, position.z);
        ScreenBox {
            x,
            y,
            width: w,
            height: h,
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

    fn null_or(field: &str) -> Option<Vec<u8>> {
        (field != "-").then(|| unhex(field))
    }

    fn rgba_hex(field: &str) -> [u8; 4] {
        u32::from_str_radix(field, 16).unwrap().to_be_bytes()
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
            "UITable" => EntryPayload::Table {
                right: f(t[2]),
                bottom: f(t[3]),
                int5: i(t[4]),
                int6: i(t[5]),
            },
            _ => EntryPayload::WinObject {
                right: f(t[2]),
                bottom: f(t[3]),
                extra_name: t.get(5).map_or_else(Vec::new, |p| unhex(p)),
                extra_float: f(t[4]),
            },
        }
    }

    const LATER: [&str; 10] = [
        "TextControl",
        "EditControl",
        "StaticControl",
        "ButtonControl",
        "ToggleButtonControl",
        "RichTextButtonControl",
        "WireStaticControl",
        "WireButtonControl",
        "ScrollControl",
        "UITable",
    ];

    fn text_of(ui: &mut Ui, id: NodeId) -> Option<&mut TextControl> {
        let n = ui.node_mut(id)?.widget_mut()?.as_any_mut();
        if n.is::<TextControlWidget>() {
            return n.downcast_mut::<TextControlWidget>().map(|w| &mut w.text);
        }
        n.downcast_mut::<EditControl>().map(|w| &mut w.text)
    }

    fn button_of(ui: &mut Ui, id: NodeId) -> Option<&mut ButtonControl> {
        let n = ui.node_mut(id)?.widget_mut()?.as_any_mut();
        if n.is::<ButtonWidget>() {
            return n.downcast_mut::<ButtonWidget>().map(|w| &mut w.button);
        }
        if n.is::<ToggleButtonWidget>() {
            return n
                .downcast_mut::<ToggleButtonWidget>()
                .map(|w| &mut w.button);
        }
        n.downcast_mut::<RichTextButtonWidget>()
            .map(|w| &mut w.button)
    }

    fn wire_of(ui: &mut Ui, id: NodeId) -> Option<&mut WireControl> {
        ui.widget_mut::<WireWidget>(id).map(|w| &mut w.wire)
    }

    /// One state line of the later controls.
    fn later_state(ui: &mut Ui, id: NodeId, class: &str, typing: i32) -> String {
        let n = ui.node(id).unwrap();
        let (flags, pos, orient) = (n.flags & 0x0F, n.position, n.orientation);
        let pending = |p: &[u16]| {
            if p.is_empty() {
                String::new()
            } else {
                units(p)
            }
        };
        match class {
            "TextControl" | "EditControl" => {
                let edit = ui.widget::<EditControl>(id).map(|e| {
                    format!(
                        ",\"ct\":\"{}\",\"cs\":\"{}\",\"ml\":\"{}\",\"ed\":\"{}\",\"dg\":\"{}\"",
                        bits(e.cursor_timer),
                        u8::from(e.cursor_shown),
                        e.max_length,
                        u8::from(e.editable),
                        u8::from(e.digits_only)
                    )
                });
                let t = text_of(ui, id).unwrap();
                format!(
                    "\"ev\":\"text\",\"flags\":\"{flags:02x}\",\"pos\":\"{}\",\"off\":\"{}\",\"w\":\"{}\",\"h\":\"{}\",\"t\":\"{}\",\"tc\":\"{}\",\"te\":\"{}\",\"ti\":\"{}\",\"tw\":\"{}\",\"typing\":\"{}\",\"pend\":\"{}\",\"blink\":\"{}\",\"ph\":\"{}\",\"be\":\"{}\",\"oc\":\"{}\",\"co\":\"{}\",\"cap\":\"{}\",\"lc\":\"{}\",\"n\":\"{typing}\"{}",
                    bits3(pos),
                    bits3(t.control.offset),
                    bits(t.width),
                    bits(t.height),
                    t.text.text().map_or_else(|| "null".to_owned(), units),
                    t.typed_count,
                    bits(t.type_elapsed),
                    bits(t.type_interval),
                    u8::from(t.typewriter),
                    u8::from(t.typing),
                    pending(&t.pending),
                    u8::from(t.blink),
                    u8::from(t.blink_phase),
                    bits(t.blink_elapsed),
                    rgba(t.override_colour),
                    u8::from(t.colour_override),
                    u8::from(t.in_capture),
                    t.line_count,
                    edit.unwrap_or_default()
                )
            }
            "StaticControl" => {
                let s = &ui.widget::<StaticControlWidget>(id).unwrap().control;
                format!(
                    "\"ev\":\"static\",\"flags\":\"{flags:02x}\",\"pos\":\"{}\",\"orient\":\"{}\",\"off\":\"{}\",\"a\":\"{}\",\"s\":\"{}\",\"f\":\"{}{}{}{}{}{}{}{}{}{}\",\"fr\":\"{}\",\"rs\":\"{}\",\"mc\":\"{}\",\"wc\":\"{}\"",
                    bits3(pos),
                    bits_n(&orient.to_words()),
                    bits3(s.control.offset),
                    bits_n(&[s.cur_roll, s.cur_pitch, s.cur_yaw]),
                    bits_n(&s.start_ypr),
                    u8::from(s.no_mesh_render),
                    u8::from(s.no_wire_render),
                    u8::from(s.no_coloring),
                    u8::from(s.field_347),
                    u8::from(s.use_colours),
                    u8::from(s.enable_rotation),
                    u8::from(s.no_clear_render),
                    u8::from(s.render_to_window),
                    u8::from(s.zclear),
                    u8::from(s.control.no_z_enable),
                    bits(s.force_radius),
                    bits(s.rotation_speed),
                    rgba(s.mesh_colour),
                    rgba(s.wire_colour)
                )
            }
            "ButtonControl" | "ToggleButtonControl" | "RichTextButtonControl" => {
                let b = button_of(ui, id).unwrap();
                format!(
                    "\"ev\":\"button\",\"flags\":\"{flags:02x}\",\"pos\":\"{}\",\"orient\":\"{}\",\"off\":\"{}\",\"b\":\"{:02x}\",\"b2\":\"{:02x}\",\"spin\":\"{}\",\"bt\":\"{}\",\"ph\":\"{}\",\"glow\":\"{:08x}\",\"cmd\":\"{}\",\"tip\":\"{}\",\"tip2\":\"{}\",\"slot\":\"{}\",\"ts\":\"{}\",\"uc\":\"{}\",\"col\":\"{}\"",
                    bits3(pos),
                    bits_n(&orient.to_words()),
                    bits3(b.control.offset),
                    b.bits,
                    b.bits2,
                    bits(b.spin_angle),
                    bits(b.blink_timer),
                    u8::from(b.blink_phase),
                    b.glow_argb,
                    b.command_id,
                    b.control.tooltip_ids,
                    b.tooltip2,
                    b.ui_sound_slot,
                    b.toggle.as_ref().map_or(-1, |t| t.state),
                    u8::from(b.use_colour),
                    rgba(b.colour)
                )
            }
            "WireStaticControl" | "WireButtonControl" => {
                let w = wire_of(ui, id).unwrap();
                format!(
                    "\"ev\":\"wire\",\"flags\":\"{flags:02x}\",\"pos\":\"{}\",\"c\":\"{}\",\"st\":\"{:02x}\",\"m\":\"{:x}\",\"bl\":\"{}\",\"ph\":\"{}\",\"bt\":\"{}\",\"bs\":\"{}\",\"cmd\":\"{}\",\"slot\":\"{}\"",
                    bits3(pos),
                    bits3(w.wire_center),
                    w.wire_state,
                    w.edge_mask,
                    u8::from(w.blink),
                    u8::from(w.blink_phase),
                    bits(w.blink_timer),
                    w.button.as_ref().map_or(-1, |b| i32::from(b.state & 7)),
                    w.button.as_ref().map_or(0, |b| b.command_id),
                    w.sound_slot
                )
            }
            "ScrollControl" => {
                let c = &ui.widget::<ScrollControlWidget>(id).unwrap().scroll;
                let x = |t: &Transform| bits_n(&t.to_words());
                format!(
                    "\"ev\":\"sctl\",\"v\":\"{}\",\"min\":\"{}\",\"max\":\"{}\",\"pg\":\"{}\",\"st\":\"{}\",\"valid\":\"{}\",\"f\":\"{}{}{}{}{}{}{}\",\"rt\":\"{}\",\"back\":\"{}\",\"next\":\"{}\",\"prev\":\"{}\",\"thumb\":\"{}\",\"ts\":\"{}\",\"te\":\"{}\"",
                    c.value,
                    c.min_range,
                    c.max_range,
                    c.page_size,
                    c.step,
                    u8::from(c.thumb_valid),
                    u8::from(c.dragging),
                    u8::from(c.paging),
                    u8::from(c.next_pressed),
                    u8::from(c.prev_pressed),
                    u8::from(c.thumb_hover),
                    u8::from(c.next_hover),
                    u8::from(c.prev_hover),
                    bits(c.repeat_time),
                    x(&c.back.xform),
                    x(&c.next.xform),
                    x(&c.prev.xform),
                    x(&c.thumb_xform),
                    x(&c.thumb_start),
                    x(&c.thumb_end)
                )
            }
            _ => {
                let t = ui.widget::<UiTable>(id).unwrap();
                format!(
                    "\"ev\":\"table\",\"w\":\"{}\",\"h\":\"{}\",\"re\":\"{}\",\"ce\":\"{}\",\"sel\":\"{} {}\",\"hov\":\"{} {}\",\"drag\":\"{}{}{} {}\",\"do\":\"{}\",\"inv\":\"{}\"",
                    bits(t.width),
                    bits(t.height),
                    bits_n(&t.row_edges),
                    bits_n(&t.column_edges),
                    u8::from(t.selected),
                    t.selected_row,
                    u8::from(t.hovered),
                    t.hovered_row,
                    u8::from(t.drag_armed),
                    u8::from(t.dragging),
                    u8::from(t.drag_column),
                    t.drag_edge,
                    bits(t.drag_offset),
                    bits_n(&t.inverse.to_words())
                )
            }
        }
    }

    fn v3(t: &[&str], k: usize) -> Vector {
        Vector::new(f(t[k]), f(t[k + 1]), f(t[k + 2]))
    }

    fn opt_bytes(t: &[&str], k: usize) -> Option<Vec<u8>> {
        null_or(t[k])
    }

    /// The Init operation of the later controls.
    fn init_control(ui: &mut Ui, id: NodeId, class: &str, t: &[&str]) {
        match class {
            "TextControl" | "EditControl" => {
                let style = opt_bytes(t, 1);
                let hp = opt_bytes(t, 5);
                ui.with_widget(id, (), |w, ui| {
                    let any = w.as_any_mut();
                    let tc = if any.is::<EditControl>() {
                        &mut any.downcast_mut::<EditControl>().unwrap().text
                    } else {
                        &mut any.downcast_mut::<TextControlWidget>().unwrap().text
                    };
                    tc.init(
                        ui,
                        id,
                        style.as_deref(),
                        v3(t, 2),
                        hp.as_deref(),
                        f(t[6]),
                        f(t[7]),
                        i(t[8]).cast_unsigned(),
                        i(t[9]),
                        i(t[10]),
                        i(t[11]) != 0,
                    );
                });
            }
            "StaticControl" => {
                let mesh = opt_bytes(t, 1);
                let hp = opt_bytes(t, 5);
                with::<StaticControlWidget, _>(ui, id, |w, ui| {
                    w.control.init(
                        ui,
                        id,
                        mesh.as_deref(),
                        v3(t, 2),
                        hp.as_deref(),
                        v3(t, 6),
                        i(t[9]) != 0,
                        i(t[10]) != 0,
                        i(t[11]) != 0,
                        i(t[12]) != 0,
                        i(t[13]) != 0,
                        i(t[14]) != 0,
                        i(t[15]) != 0,
                        i(t[16]),
                        i(t[17]),
                        i(t[18]) != 0,
                        [f(t[19]), f(t[20]), f(t[21])],
                    );
                });
            }
            "ButtonControl" | "ToggleButtonControl" | "RichTextButtonControl" => {
                let (mesh, mesh2, sound, key, hp) = (
                    opt_bytes(t, 1),
                    opt_bytes(t, 2),
                    opt_bytes(t, 3),
                    opt_bytes(t, 4),
                    opt_bytes(t, 8),
                );
                let command = if class == "ToggleButtonControl" {
                    0
                } else {
                    i(t[18])
                };
                ui.with_widget(id, (), |w, ui| {
                    let any = w.as_any_mut();
                    let b = if any.is::<ButtonWidget>() {
                        &mut any.downcast_mut::<ButtonWidget>().unwrap().button
                    } else if any.is::<ToggleButtonWidget>() {
                        &mut any.downcast_mut::<ToggleButtonWidget>().unwrap().button
                    } else {
                        &mut any.downcast_mut::<RichTextButtonWidget>().unwrap().button
                    };
                    b.init(
                        ui,
                        id,
                        mesh.as_deref(),
                        mesh2.as_deref(),
                        sound.as_deref(),
                        key.as_deref(),
                        v3(t, 5),
                        hp.as_deref(),
                        v3(t, 9),
                        i(t[12]) as u8,
                        i(t[13]) as u8,
                        i(t[14]) as u8,
                        i(t[15]) != 0,
                        i(t[16]),
                        i(t[17]),
                        command,
                    );
                });
            }
            "WireStaticControl" => {
                with::<WireWidget, _>(ui, id, |w, ui| {
                    w.wire.init_static(
                        ui,
                        id,
                        v3(t, 1),
                        v3(t, 4),
                        i(t[7]) as u8,
                        i(t[8]) != 0,
                        i(t[9]),
                        i(t[10]).cast_unsigned(),
                    );
                });
            }
            "WireButtonControl" => {
                let sound = opt_bytes(t, 1);
                with::<WireWidget, _>(ui, id, |w, ui| {
                    w.wire.init_button(
                        ui,
                        id,
                        sound.as_deref(),
                        v3(t, 2),
                        v3(t, 5),
                        i(t[8]) as u8,
                        i(t[9]) != 0,
                        i(t[10]),
                        i(t[11]),
                        i(t[12]).cast_unsigned(),
                    );
                });
            }
            "ScrollControl" => {
                with::<ScrollControlWidget, _>(ui, id, |w, ui| {
                    for (k, part) in [PartId::Back, PartId::Next, PartId::Prev]
                        .into_iter()
                        .enumerate()
                    {
                        let b = 1 + k * 9;
                        let mesh = opt_bytes(t, b);
                        let hp = opt_bytes(t, b + 4);
                        w.scroll.init_part(
                            ui,
                            id,
                            part,
                            mesh.as_deref(),
                            v3(t, b + 1),
                            hp.as_deref(),
                            v3(t, b + 5),
                            i(t[b + 8]) != 0,
                        );
                    }
                    let b = 28;
                    let mesh = opt_bytes(t, b);
                    let hp0 = opt_bytes(t, b + 7);
                    let hp1 = opt_bytes(t, b + 8);
                    w.scroll.init_thumb(
                        ui,
                        id,
                        mesh.as_deref(),
                        v3(t, b + 1),
                        v3(t, b + 4),
                        hp0.as_deref(),
                        hp1.as_deref(),
                        v3(t, b + 9),
                        i(t[b + 12]) != 0,
                        (i(t[b + 13]), i(t[b + 14]), i(t[b + 15])),
                        i(t[b + 16]) != 0,
                        i(t[b + 17]) != 0,
                    );
                });
            }
            _ => panic!("no Init for {class}"),
        }
    }

    /// `SetProperty` with integers, as a typed property.
    fn int_property(m: i32, a: i32, b: i32) -> Property<'static> {
        match m {
            1 => Property::Hidden(a != 0),
            2 => Property::Inactive(a != 0),
            3 => Property::ToolTip(a),
            0x37 => Property::ScrollRange(a, b),
            0x38 => Property::ScrollValue(a),
            0x3B => Property::GetScrollValue,
            _ => Property::Other(m, a as usize, b as usize),
        }
    }

    /// The operations of the later controls; false when it is not one.
    fn later_op(ui: &mut Ui, id: NodeId, class: &str, log: &Shared, t: &[&str], ctx: &Ctx) -> bool {
        let prop =
            |ui: &mut Ui, p: Property<'_>| ui.with_widget(id, 0, |w, ui| w.property(ui, id, p));
        match t[0] {
            "init" => init_control(ui, id, class, t),
            "enter" => {
                let ray = ui.mouse_ray();
                ui.with_widget(id, (), |w, ui| w.mouse_enter(ui, id, ray));
            }
            "inactive" => ui.with_widget(id, (), |w, ui| w.update_inactive(ui, id)),
            "key" => {
                let mut mods = KeyMods(i(t[2]) as u8);
                let r = ui.with_widget(id, false, |w, ui| {
                    w.key_event(ui, id, i(t[1]).cast_unsigned(), &mut mods)
                });
                result(log, u8::from(r));
            }
            "time" => ctx.elapsed.set(f64::from(f(t[1]))),
            "gopt" => ctx.options.borrow_mut()[i(t[1]) as usize] = i(t[2]),
            "prop" => {
                let m = i32::from_str_radix(t[1], 16).unwrap();
                let r = prop(ui, int_property(m, i(t[2]), i(t[3])));
                result(log, r);
            }
            "offset" => {
                let r = prop(ui, Property::Offset(v3(t, 1)));
                result(log, r);
            }
            "text" => {
                let w = wide(t[1]);
                let r = prop(ui, Property::SetText(Some(&w)));
                result(log, r);
            }
            "colour" => {
                let r = prop(ui, Property::Colour(rgba_hex(t[1]), i(t[2]) == 1));
                result(log, r);
            }
            "colours" => {
                let r = prop(ui, Property::Colours(rgba_hex(t[1]), rgba_hex(t[2])));
                result(log, r);
            }
            "getcmd" | "gettoggle" => {
                let mut out = -7;
                if t[0] == "getcmd" {
                    prop(ui, Property::GetCommand(&mut out));
                } else {
                    prop(ui, Property::GetToggleState(&mut out));
                }
                result(log, out);
            }
            "sounds" => {
                let (s0, s1) = (null_or(t[1]), null_or(t[2]));
                prop(ui, Property::ToggleSounds(s0.as_deref(), s1.as_deref()));
            }
            "tset" | "ids" | "width" | "cached" | "number" => {
                let edit = ui.widget::<EditControl>(id).is_some();
                let run = |tc: &mut TextControl, ui: &mut Ui| match t[0] {
                    "tset" => tc.set_text(ui, id, Some(&wide(t[1]))),
                    "ids" => tc.set_ids(ui, id, i(t[1]).cast_unsigned()),
                    "width" => tc.set_width(ui, id, f(t[1])),
                    "cached" => tc.set_cached_draw(i(t[1]) != 0, i(t[2]) != 0),
                    _ => tc.set_number_text(ui, id, &unhex(t[1])),
                };
                if edit {
                    with::<EditControl, _>(ui, id, |w, ui| run(&mut w.text, ui));
                } else {
                    with::<TextControlWidget, _>(ui, id, |w, ui| run(&mut w.text, ui));
                }
            }
            "cedge" | "redge" => {
                let r = with::<UiTable, _>(ui, id, |w, ui| {
                    if t[0] == "cedge" {
                        w.set_column_edge(ui, id, i(t[1]).cast_unsigned(), f(t[2]))
                    } else {
                        w.set_row_edge(ui, id, i(t[1]).cast_unsigned(), f(t[2]))
                    }
                });
                result(log, u8::from(r));
            }
            "rows" => {
                let w = ui.widget_mut::<UiTable>(id).unwrap();
                for (k, row) in w.rows.iter_mut().enumerate() {
                    if 2 + k * 2 >= t.len() {
                        break;
                    }
                    row.hoverable = i(t[1 + k * 2]) != 0;
                    row.selectable = i(t[2 + k * 2]) != 0;
                }
            }
            "state" => {
                let typing = ui.text_typing_count;
                let line = later_state(ui, id, class, typing);
                ev(log, &line);
            }
            _ => return false,
        }
        true
    }

    /// What the operations share across a case.
    struct Ctx {
        elapsed: Rc<Cell<f64>>,
        options: Rc<RefCell<[i32; 0x20]>>,
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

    /// The operations of the first control classes.
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

    // ---- the rich-text windows (the fifth increment) ------------------------

    const RICH: [&str; 4] = [
        "RichTextEditWin",
        "UITextEdit",
        "UIFastTable",
        "UIScrollTable",
    ];

    /// A node as `rich_text.jsonl` prints it; a reference as r(its list).
    fn rich_node(node: &RdlNode) -> String {
        match node {
            RdlNode::Attributes(a) => {
                format!("a:{:08x}:{:08x}:{:08x}", a.attrs, a.mask, a.default_mask)
            }
            RdlNode::Text(text) => format!("t:{}", units(text)),
            RdlNode::Paragraph => "n".to_owned(),
            RdlNode::Clip(r) => format!("c:{}:{}:{}:{}", r.left, r.top, r.right, r.bottom),
            RdlNode::Ref(list) => format!("r({})", rich_list(list)),
            _ => "other".to_owned(),
        }
    }

    fn rich_list(list: &RenderDisplayList) -> String {
        list.nodes
            .iter()
            .map(rich_node)
            .collect::<Vec<_>>()
            .join(" ")
    }

    fn edit_of(ui: &Ui, id: NodeId) -> &RichTextEditWin {
        let w = ui.node(id).unwrap().widget().unwrap().as_any();
        w.downcast_ref::<RichTextEditWin>()
            .unwrap_or_else(|| &w.downcast_ref::<UiTextEdit>().unwrap().edit)
    }

    fn with_edit<R>(
        ui: &mut Ui,
        id: NodeId,
        f: impl FnOnce(&mut RichTextEditWin, &mut Ui) -> R,
    ) -> R {
        let mut out = None;
        ui.with_widget(id, (), |w, ui| {
            let any = w.as_any_mut();
            let edit = if any.is::<RichTextEditWin>() {
                any.downcast_mut::<RichTextEditWin>().unwrap()
            } else {
                &mut any.downcast_mut::<UiTextEdit>().unwrap().edit
            };
            out = Some(f(edit, ui));
        });
        out.unwrap()
    }

    fn table_of(ui: &Ui, id: NodeId) -> &UiFastTable {
        let w = ui.node(id).unwrap().widget().unwrap().as_any();
        w.downcast_ref::<UiFastTable>()
            .unwrap_or_else(|| &w.downcast_ref::<UiScrollTable>().unwrap().table)
    }

    fn with_table<R>(ui: &mut Ui, id: NodeId, f: impl FnOnce(&mut UiFastTable, &mut Ui) -> R) -> R {
        let mut out = None;
        ui.with_widget(id, (), |w, ui| {
            let any = w.as_any_mut();
            let table = if any.is::<UiFastTable>() {
                any.downcast_mut::<UiFastTable>().unwrap()
            } else {
                &mut any.downcast_mut::<UiScrollTable>().unwrap().table
            };
            out = Some(f(table, ui));
        });
        out.unwrap()
    }

    fn edit_state(ui: &Ui, id: NodeId) -> String {
        let e = edit_of(ui, id);
        let b = &e.win.base;
        let cells = e
            .cells
            .iter()
            .map(|c| format!("{:04x}:{:08x}", c.ch, c.attrs))
            .collect::<Vec<_>>()
            .join(" ");
        format!(
            "\"ev\":\"edit\",\"flags\":\"{:x}\",\"caret\":\"{} {} {}\",\"blink\":\"{} {}\",\"attrs\":\"{:08x}\",\"cells\":\"{cells}\",\"content\":\"{}\",\"dirty\":\"{}{}\",\"modes\":\"{}{}{} {} {} {} {:04x}\",\"size\":\"{} {}\"",
            ui.node(id).unwrap().flags & 0xF,
            e.caret,
            e.field_4a0,
            e.field_4a4,
            u8::from(e.caret_visible),
            bits(e.caret_timer),
            e.current_attrs,
            rich_list(&e.win.content),
            u8::from(b.layout_dirty),
            u8::from(b.surface_dirty),
            u8::from(e.editable),
            u8::from(e.fixed_style),
            u8::from(e.password_mode),
            e.case_mode,
            e.max_length,
            u8::from(e.filter_is_allow_list),
            e.replacement,
            bits(b.norm_width),
            bits(b.norm_height),
        )
    }

    fn fast_table_state(ui: &Ui, id: NodeId) -> String {
        let t = table_of(ui, id);
        let b = &t.base;
        let rows = t
            .rows
            .iter()
            .map(|r| {
                format!(
                    "{}{}{}:{}",
                    u8::from(r.hoverable),
                    u8::from(r.selectable),
                    u8::from(r.row_line),
                    r.group_id
                )
            })
            .collect::<Vec<_>>()
            .join(" ");
        let mut inv = t.inverse.rotation.to_words().to_vec();
        inv.extend_from_slice(&t.inverse.translation.to_words());
        format!(
            "\"ev\":\"ftable\",\"w\":\"{}\",\"h\":\"{}\",\"re\":\"{}\",\"ce\":\"{}\",\"rows\":\"{rows}\",\"cells\":\"{}\",\"sel\":\"{} {}\",\"cell\":\"{} {}\",\"hov\":\"{} {}\",\"drag\":\"{}{}{} {}\",\"do\":\"{}\",\"inv\":\"{}\",\"tips\":\"{} {}\",\"hdr\":\"{}\",\"dirty\":\"{}{}\",\"size\":\"{} {}\"",
            bits(t.width),
            bits(t.height),
            bits_n(&t.row_edges),
            bits_n(&t.column_edges),
            t.cells.len(),
            u8::from(t.selected),
            t.selected_row,
            t.hover_row,
            t.hover_column,
            u8::from(t.hovered),
            t.hovered_row,
            u8::from(t.drag_armed),
            u8::from(t.dragging),
            u8::from(t.drag_column),
            t.drag_edge,
            bits(t.drag_offset),
            bits_n(&inv),
            t.tip_mode,
            t.tips.len(),
            t.header_rows,
            u8::from(b.layout_dirty),
            u8::from(b.surface_dirty),
            bits(b.norm_width),
            bits(b.norm_height),
        )
    }

    fn rich_payload(t: &[&str]) -> Option<EntryPayload> {
        match t[1] {
            "UITextEdit" => Some(EntryPayload::Text {
                right: f(t[2]),
                bottom: f(t[3]),
                caption: t.get(4).map_or_else(Vec::new, |h| wide(h)),
            }),
            "UIFastTable" | "UIScrollTable" => Some(EntryPayload::Table {
                right: f(t[2]),
                bottom: f(t[3]),
                int5: i(t[4]),
                int6: i(t[5]),
            }),
            _ => None,
        }
    }

    /// The operations of the rich-text windows; false when the operation
    /// is not one of them.
    fn rich_op(ui: &mut Ui, id: NodeId, class: &str, log: &Shared, t: &[&str]) -> bool {
        let edit = class == "RichTextEditWin" || class == "UITextEdit";
        if edit {
            let record = |t: &[&str]| -> (u32, HudRecord) {
                let mut r = HudRecord::default();
                let msg = match t[0] {
                    "ekey" => {
                        r.key = i(t[1]).cast_unsigned();
                        r.modifiers = i(t[2]).cast_unsigned();
                        r.down = i(t[3]) != 0;
                        1
                    }
                    "echar" => {
                        r.wparam = i(t[1]).cast_unsigned();
                        2
                    }
                    _ => {
                        r.message = i(t[1]).cast_unsigned();
                        0xF
                    }
                };
                (msg, r)
            };
            match t[0] {
                "ekey" | "echar" | "ecancel" => {
                    let (msg, r) = record(t);
                    let v = with_edit(ui, id, |e, ui| e.on_event(ui, id, msg, &r));
                    result(log, u8::from(v));
                }
                "settext" => with_edit(ui, id, |e, _| e.set_text(&wide(t[1]), None)),
                "settexta" => {
                    let attrs = u32::from_str_radix(t[2], 16).unwrap();
                    with_edit(ui, id, |e, _| e.set_text(&wide(t[1]), Some(attrs)));
                }
                "cleartext" => with_edit(ui, id, |e, ui| e.clear_text(ui, id)),
                "gettext" => {
                    let (text, r) = edit_of(ui, id).get_text(i(t[1]));
                    ev(
                        log,
                        &format!("\"ev\":\"text\",\"r\":\"{r}\",\"t\":\"{}\"", units(&text)),
                    );
                }
                "caret" => with_edit(ui, id, |e, _| e.set_caret(i(t[1]))),
                "maxlen" => with_edit(ui, id, |e, _| e.set_max_length(i(t[1]))),
                "fixed" => with_edit(ui, id, |e, _| e.set_fixed_style(i(t[1]) != 0)),
                "password" => with_edit(ui, id, |e, _| e.set_password_mode(i(t[1]) != 0)),
                "case" => with_edit(ui, id, |e, _| e.set_case_mode(i(t[1]))),
                "allow" => with_edit(ui, id, |e, _| e.set_filter_is_allow_list(i(t[1]) != 0)),
                "filter" => with_edit(ui, id, |e, _| e.add_filter_chars(&wide(t[1]))),
                "repl" => {
                    let ch = u16::from_str_radix(t[1], 16).unwrap();
                    with_edit(ui, id, |e, _| e.set_replacement_char(ch));
                }
                "editable" => with_edit(ui, id, |e, ui| e.set_editable(ui, id, i(t[1]) != 0)),
                "activate" => with_edit(ui, id, |e, ui| e.activate(ui, id)),
                "deactivate" => with_edit(ui, id, |e, ui| e.deactivate(ui, id, i(t[1]) != 0)),
                "attrs" => {
                    let attrs = u32::from_str_radix(t[1], 16).unwrap();
                    with_edit(ui, id, |e, _| e.current_attrs = attrs);
                }
                "fmask" => {
                    let mask = i32::from_str_radix(t[1], 16).unwrap();
                    with_edit(ui, id, |e, _| e.format_mask = mask);
                }
                "prefix" => {
                    let mut list = RenderDisplayList::new();
                    list.push(RdlNode::Text(wide(t[1])));
                    with_edit(ui, id, |e, _| {
                        e.prefix = Rc::new(list);
                        e.rebuild_display();
                    });
                }
                "fixedlist" => {
                    let attrs = u32::from_str_radix(t[1], 16).unwrap();
                    let mut list = RenderDisplayList::new();
                    list.push(RdlNode::Attributes(TextAttributes::new(
                        attrs,
                        0xFFFF_FFFF,
                        0,
                    )));
                    with_edit(ui, id, |e, _| {
                        e.fixed_style_list = Rc::new(list);
                        e.rebuild_display();
                    });
                }
                "state" => {
                    let line = edit_state(ui, id);
                    ev(log, &line);
                }
                _ => return false,
            }
            return true;
        }
        let scroll = class == "UIScrollTable";
        match t[0] {
            "cedge" => {
                let r = with_table(ui, id, |w, _| {
                    w.set_column_edge(i(t[1]).cast_unsigned(), f(t[2]))
                });
                result(log, u8::from(r));
            }
            "redge" => {
                let r = with_table(ui, id, |w, _| {
                    w.set_row_edge(i(t[1]).cast_unsigned(), f(t[2]))
                });
                result(log, u8::from(r));
            }
            "frows" => with_table(ui, id, |w, _| {
                for (index, row) in w.rows.iter_mut().enumerate() {
                    let k = 1 + index * 4;
                    if k + 3 >= t.len() {
                        break;
                    }
                    row.hoverable = i(t[k]) != 0;
                    row.selectable = i(t[k + 1]) != 0;
                    row.row_line = i(t[k + 2]) != 0;
                    row.group_id = i(t[k + 3]).cast_unsigned();
                }
            }),
            "selrow" => {
                let r = with_table(ui, id, |w, ui| {
                    w.select_row(ui, id, i(t[1]).cast_unsigned())
                });
                result(log, u8::from(r));
            }
            "clearsel" => with_table(ui, id, |w, _| w.clear_selection()),
            "getsel" => {
                let r = table_of(ui, id).selected_row();
                result(log, format!("{} {}", u8::from(r.is_some()), r.unwrap_or(0)));
            }
            "hdr" => with_table(ui, id, |w, _| w.set_header_rows(i(t[1]).cast_unsigned())),
            "borders" => with_table(ui, id, |w, _| {
                w.set_borders(i(t[1]) != 0, i(t[2]) != 0, i(t[3]) != 0, i(t[4]) != 0);
            }),
            "collines" => with_table(ui, id, |w, _| w.set_draw_column_lines(i(t[1]) != 0)),
            "highlights" => with_table(ui, id, |w, _| w.set_row_highlights(i(t[1]) != 0)),
            "resizable" => with_table(ui, id, |w, _| w.set_resizable(i(t[1]) != 0, i(t[2]) != 0)),
            "grab" => with_table(ui, id, |w, _| w.set_grab_distance(f(t[1]))),
            "margins" => with_table(ui, id, |w, _| w.set_cell_margins(i(t[1]), i(t[2]))),
            "tipmode" => with_table(ui, id, |w, _| w.set_tip_mode(id, i(t[1]))),
            "wtip" => with_table(ui, id, |w, _| w.base.hover.ids_alt = i(t[1])),
            "coltip" => with_table(ui, id, |w, _| w.tips[i(t[1]) as usize].ids_alt = i(t[2])),
            "rtip" => with_table(ui, id, |w, _| {
                w.row_tip_mut(i(t[1]).cast_unsigned()).unwrap().ids_alt = i(t[2]);
            }),
            "ctip" => with_table(ui, id, |w, _| {
                w.cell_tip_mut(i(t[1]).cast_unsigned(), i(t[2]).cast_unsigned())
                    .unwrap()
                    .ids_alt = i(t[3]);
            }),
            "tooltip" => with_table(ui, id, |w, _| w.base.tooltip_enabled = i(t[1]) != 0),
            "clearcell" => {
                let r = with_table(ui, id, |w, _| {
                    w.clear_cell(i(t[1]).cast_unsigned(), i(t[2]).cast_unsigned())
                });
                result(log, u8::from(r));
            }
            "span" => with_table(ui, id, |w, _| {
                w.set_cell_span_to_end(
                    i(t[1]).cast_unsigned(),
                    i(t[2]).cast_unsigned(),
                    i(t[3]) != 0,
                );
            }),
            "invalidate" => with_table(ui, id, |w, _| {
                w.invalidate_cell(i(t[1]).cast_unsigned(), i(t[2]).cast_unsigned());
            }),
            "setedges" => {
                let mut at = 1;
                let mut edges = TableEdges::default();
                let rows = i(t[at]);
                at += 1;
                for _ in 0..rows {
                    edges.rows.push(f(t[at]));
                    at += 1;
                }
                let cols = i(t[at]);
                at += 1;
                for _ in 0..cols {
                    edges.cols.push(f(t[at]));
                    at += 1;
                }
                with_table(ui, id, |w, _| {
                    w.set_edges(&edges);
                });
            }
            "items" if scroll => {
                with::<UiScrollTable, _>(ui, id, |w, ui| {
                    w.set_item_count(ui, i(t[1]).cast_unsigned(), i(t[2]).cast_unsigned());
                });
            }
            "first" if scroll => {
                let r = ui.widget::<UiScrollTable>(id).unwrap().first_item(ui);
                result(log, r);
            }
            "callback" if scroll => {
                let cb_log = log.clone();
                let callback: ScrollTableCallback = Box::new(move |first, rows| {
                    ev(
                        &cb_log,
                        &format!("\"ev\":\"cb\",\"first\":\"{first}\",\"rows\":\"{rows}\""),
                    );
                });
                with::<UiScrollTable, _>(ui, id, |w, ui| w.set_callback(ui, Some(callback)));
            }
            "barvalue" if scroll => {
                let bar = ui.widget::<UiScrollTable>(id).unwrap().scroll_bar.unwrap();
                ui.widget_mut::<UiScroll>(bar).unwrap().set_value(f(t[1]));
            }
            "scrolled" if scroll => {
                let bar = ui.widget::<UiScrollTable>(id).unwrap().scroll_bar.unwrap();
                let r = ui.send_child_message(id, &ChildMessage::ScrollChanged { from: bar });
                result(log, u8::from(r));
            }
            "gap" if scroll => {
                with::<UiScrollTable, _>(ui, id, |w, ui| w.set_scroll_bar_gap(ui, id, f(t[1])));
            }
            "bstate" if scroll => {
                // The bar's flags as the other states print them: the low four bits.
                let bar = ui.widget::<UiScrollTable>(id).unwrap().scroll_bar.unwrap();
                let saved = ui.node(bar).unwrap().flags;
                ui.node_mut(bar).unwrap().flags = saved & 0xF;
                let line = scroll_state(ui, bar, ui.widget::<UiScroll>(bar).unwrap());
                ui.node_mut(bar).unwrap().flags = saved;
                ev(log, &line);
            }
            "state" => {
                let line = fast_table_state(ui, id);
                ev(log, &line);
                if scroll {
                    let s = ui.widget::<UiScrollTable>(id).unwrap();
                    ev(
                        log,
                        &format!(
                            "\"ev\":\"stable\",\"items\":\"{}\",\"fixed\":\"{}\"",
                            s.item_count, s.fixed_rows
                        ),
                    );
                }
            }
            _ => return false,
        }
        true
    }

    pub fn lines(case: usize, line: &str) -> Vec<String> {
        let log: Shared = Rc::new(RefCell::new(Log {
            case,
            lines: Vec::new(),
            muted: false,
        }));
        let mut ui = Ui::new(Surface::new(640, 480));
        let instances = Rc::new(Cell::new(0));
        ui.mesh_loader = Some(Box::new(RecLoader(log.clone(), instances)));
        let ctx = Ctx {
            elapsed: Rc::new(Cell::new(0.0)),
            options: Rc::new(RefCell::new([1; 0x20])),
        };
        let child = Rc::new(RefCell::new(None));
        ui.services = Box::new(RecServices {
            log: log.clone(),
            elapsed: ctx.elapsed.clone(),
            child: child.clone(),
            focus: None,
            focus_stack: Vec::new(),
        });
        ui.text_projection = Some(Box::new(Fixed));
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
            options: [1; 0x20],
            log: log.clone(),
        };
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
        let tip = ui.alloc(Some(Box::new(RecTip {
            log: log.clone(),
            child: child.clone(),
        })));
        ui.tooltip = Some(tip);
        let mut object: Option<NodeId> = None;
        let mut class = String::new();
        for op in line.split(" ; ") {
            let t: Vec<&str> = op.split(' ').collect();
            host.options = *ctx.options.borrow();
            if t[0] == "new" {
                class = t[1].to_owned();
                let later = LATER.contains(&class.as_str()) || class == "RichTextEditWin";
                object = if let Some(payload) = rich_payload(&t) {
                    ui.create_by_class(
                        Some(b"C"),
                        class.as_bytes(),
                        Some(parent),
                        &mut Payload::Page(&payload),
                    )
                } else if later && class != "UITable" {
                    ui.create_by_class(
                        Some(b"C"),
                        class.as_bytes(),
                        Some(parent),
                        &mut Payload::None,
                    )
                } else {
                    let payload = payload(&t);
                    ui.create_by_class(
                        Some(b"C"),
                        class.as_bytes(),
                        Some(parent),
                        &mut Payload::Page(&payload),
                    )
                };
                if let Some(id) = object
                    && let Some(tc) = text_of(&mut ui, id)
                {
                    tc.text = Box::new(RecText {
                        log: log.clone(),
                        text: None,
                        number: None,
                        font: None,
                        slots: false,
                    });
                }
                *child.borrow_mut() = object;
                ev(
                    &log,
                    &format!("\"ev\":\"new\",\"ok\":\"{}\"", u8::from(object.is_some())),
                );
                continue;
            }
            let Some(id) = object else { continue };
            if LATER.contains(&class.as_str()) && later_op(&mut ui, id, &class, &log, &t, &ctx) {
                continue;
            }
            if RICH.contains(&class.as_str()) && rich_op(&mut ui, id, &class, &log, &t) {
                continue;
            }
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
        log.borrow_mut().muted = true;
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
