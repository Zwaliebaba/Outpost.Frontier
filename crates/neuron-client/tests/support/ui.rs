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
