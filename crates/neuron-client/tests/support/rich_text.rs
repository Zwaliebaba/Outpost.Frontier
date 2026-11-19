//! The rich-text suite (`CppSource/Oracle/oracle_rich_text.cpp` writes
//! the same lines) over `tests/fixtures/oracle/rich_text.list`, whose ops
//! `make_text_fixtures.py` describes.
//!
//! ```text
//! {"case":"<n>","style":"<id>","nodes":"<node> ..."}
//! {"case":"<n>","result":"<0|1>","height":"<h>","laid":"<node> ...","layout":"<log>",
//!  "after":"<context>","exec":"<log>","exec_after":"<context>","dims":"<w>:<h>:<d>",
//!  "dims_log":"<log>","count":"<n>","pos":"<index>=<h>:<v>:<d> ..."}
//! {"case":"<n>","lex":"<result>","self":"<result>"}
//! ```
//!
//! Nodes are written as the ops that build them (a reference node as
//! `ref`). A log is what the context was asked to do, in order:
//! `T<highlight>@<pen h>,<pen v>/<origin h>,<origin v>#<attributes>:<units>`
//! for text, `I<draw>@<pen>/<origin>:<w bits>:<h bits>` (or `I<draw>:-`)
//! for images, `C<left>,<top>,<right>,<bottom>` for clip rectangles. A
//! context is `<attributes>:<pen h>:<pen v>:<attribute stack>:<state
//! stack>`. Floats are their bits in hex.
//!
//! The rich fonts suite (`CppSource/Oracle/oracle_rich_fonts.cpp`, the
//! client's `LoadRichFontsFromIni` compiled in) reads the INI files of
//! `tests/fixtures/oracle/rich_fonts.list`: [`rich_fonts_lines`].
//!
//! ```text
//! {"case":"<n>","fonts":"<index>:<face hex>:<size>:<bold><italic><underline> ..."}
//! {"case":"<n>","probe":"<token>","id":"<id>","nodes":"<node> ..."}
//! ```

use std::fmt::Write as _;
use std::rc::Rc;

use neuron_client::text::layout::list::LayoutParams;
use neuron_client::text::layout::{
    StyleCollection, TextRenderContext, TrcState, VisualSize, layout_rich_text, lex_compare,
};
use neuron_client::text::rdl::{
    ClipRect, Justify, Position, RdlNode, RenderDisplayList, RichImage, StackOp, TextAttributes,
};
use neuron_core::num::trunc_to_i32;

/// The fake font of the suite: a font byte B has a cell of
/// `10 + 2 * ((B >> 3) & 7)` pixels, three quarters of it ascent, and its
/// bold bit widens every character but NUL by one.
fn cell_of(attrs: u32) -> i32 {
    10 + 2 * ((attrs & 0xFF) >> 3 & 7) as i32
}

fn ascent_of(attrs: u32) -> i32 {
    cell_of(attrs) * 3 / 4
}

fn advance_of(attrs: u32, unit: u16) -> i32 {
    let cell = cell_of(attrs);
    if unit == 0 {
        return 0;
    }
    let advance = match unit {
        0x20 => cell / 3,
        0x69 | 0x6C | 0x2E | 0x2C | 0x27 | 0x7C => (cell / 4).max(1),
        0x4D | 0x57 => cell,
        _ => cell / 2,
    };
    advance + (attrs & 1) as i32
}

fn width_of(attrs: u32, text: &[u16]) -> i32 {
    text.iter().map(|&u| advance_of(attrs, u)).sum()
}

fn units_hex(units: &[u16]) -> String {
    units.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

/// `FRecordingTRC`: the fake font and a log of what it was asked.
#[derive(Default)]
pub struct RecordingTrc {
    state: TrcState,
    clip: ClipRect,
    log: Vec<String>,
}

impl RecordingTrc {
    pub fn begin(&mut self, attrs: u32) {
        self.state.current = attrs;
        self.state.default = attrs;
        self.state.attr_stack.clear();
        self.set_pos(0, 0);
        self.state.origin_h = 0;
        self.state.origin_v = 0;
        self.log.clear();
    }

    fn after(&self) -> String {
        let s = &self.state;
        format!(
            "{:08x}:{}:{}:{}:{}",
            s.current,
            s.pos_h,
            s.pos_v,
            s.attr_stack.len(),
            s.state_stack.len()
        )
    }

    fn take_log(&mut self) -> String {
        let log = self.log.join(" ");
        self.log.clear();
        log
    }
}

impl TextRenderContext for RecordingTrc {
    fn trc(&self) -> &TrcState {
        &self.state
    }

    fn trc_mut(&mut self) -> &mut TrcState {
        &mut self.state
    }

    fn cell_width(&mut self) -> i32 {
        cell_of(self.state.current) + (self.state.current & 1) as i32
    }

    fn cell_height(&mut self) -> i32 {
        cell_of(self.state.current)
    }

    fn text_visual_size(&mut self, text: Option<&[u16]>, size: &mut VisualSize) {
        let Some(text) = text else {
            size.ascent = 0;
            size.descent = 0;
            return;
        };
        let attrs = self.state.current;
        size.width = width_of(attrs, text);
        size.height = cell_of(attrs);
        size.ascent = ascent_of(attrs);
        size.descent = size.height - size.ascent;
    }

    fn font_metrics(&mut self) -> (i32, i32) {
        let ascent = ascent_of(self.state.current);
        (ascent, cell_of(self.state.current) - ascent)
    }

    fn text_fit(&mut self, text: Option<&[u16]>, max_width: i32) -> i32 {
        let Some(text) = text.filter(|t| !t.is_empty()) else {
            return 0;
        };
        let mut total = 0;
        let mut fit = 0;
        for &unit in text {
            total += advance_of(self.state.current, unit);
            if total > max_width {
                break;
            }
            fit += 1;
        }
        fit
    }

    fn execute_text(&mut self, text: Option<&[u16]>, highlight: bool) {
        let Some(text) = text else {
            self.log.push("T:-".to_owned());
            return;
        };
        let s = &self.state;
        self.log.push(format!(
            "T{}@{},{}/{},{}#{:08x}:{}",
            u8::from(highlight),
            s.pos_h,
            s.pos_v,
            s.origin_h,
            s.origin_v,
            s.current,
            units_hex(text)
        ));
        self.state.pos_h += width_of(self.state.current, text);
    }

    fn execute_image(&mut self, image: Option<&RichImage>, draw: bool) {
        let Some(image) = image else {
            self.log.push(format!("I{}:-", u8::from(draw)));
            return;
        };
        let s = &self.state;
        self.log.push(format!(
            "I{}@{},{}/{},{}:{:08x}:{:08x}",
            u8::from(draw),
            s.pos_h,
            s.pos_v,
            s.origin_h,
            s.origin_v,
            image.width.to_bits(),
            image.height.to_bits()
        ));
        self.state.pos_h = trunc_to_i32(f64::from(image.width) + f64::from(self.state.pos_h));
    }

    fn set_clip_rect(&mut self, rect: ClipRect) {
        self.log.push(format!(
            "C{},{},{},{}",
            rect.left, rect.top, rect.right, rect.bottom
        ));
        self.clip = rect;
    }

    fn clip_rect(&mut self) -> ClipRect {
        self.clip
    }

    fn image_scale(&self) -> f32 {
        1.0
    }
}

fn wide(hex: &str) -> Vec<u16> {
    (0..hex.len() / 4)
        .map(|i| u16::from_str_radix(&hex[i * 4..i * 4 + 4], 16).unwrap())
        .collect()
}

fn hex_word(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap()
}

/// The nodes of the ops from `tokens[*at]` to a closing `]` or the end.
fn build(tokens: &[&str], at: &mut usize) -> RenderDisplayList {
    let mut list = RenderDisplayList::new();
    while *at < tokens.len() {
        let token = tokens[*at];
        *at += 1;
        if token == "]" {
            break;
        }
        if token == "[" {
            let inner = build(tokens, at);
            list.push(RdlNode::Ref(Rc::new(inner)));
            continue;
        }
        let f: Vec<&str> = token.split(':').collect();
        let node = match f[0] {
            "t" => RdlNode::Text(wide(f.get(1).copied().unwrap_or(""))),
            "p" => RdlNode::TextPtr(wide(f.get(1).copied().unwrap_or(""))),
            "a" => RdlNode::Attributes(TextAttributes::new(
                hex_word(f[1]),
                hex_word(f[2]),
                hex_word(f[3]),
            )),
            "s" => RdlNode::AttribStack(StackOp::from_value(f[1].parse().unwrap())),
            "x" => RdlNode::Position(Position {
                rel_h: f[1] == "1",
                rel_v: f[2] == "1",
                pos_h: f32::from_bits(hex_word(f[3])),
                pos_v: f32::from_bits(hex_word(f[4])),
            }),
            "n" => RdlNode::Paragraph,
            "j" => RdlNode::Justify(Justify::from_value(f[1].parse().unwrap())),
            "c" => RdlNode::Clip(ClipRect {
                left: f[1].parse().unwrap(),
                top: f[2].parse().unwrap(),
                right: f[3].parse().unwrap(),
                bottom: f[4].parse().unwrap(),
            }),
            "y" => RdlNode::Style(f[1].parse().unwrap()),
            "0" => RdlNode::Null,
            "i" if f[1] == "-" => RdlNode::Image(None),
            "i" => RdlNode::Image(Some(Rc::new(RichImage::sized(
                f32::from_bits(hex_word(f[1])),
                f32::from_bits(hex_word(f[2])),
            )))),
            other => panic!("bad token {other}"),
        };
        list.push(node);
    }
    list
}

fn node_text(node: &RdlNode) -> String {
    match node {
        RdlNode::Attributes(a) => {
            format!("a:{:08x}:{:08x}:{:08x}", a.attrs, a.mask, a.default_mask)
        }
        RdlNode::Text(text) => format!("t:{}", units_hex(text)),
        RdlNode::TextPtr(text) => format!("p:{}", units_hex(text)),
        RdlNode::AttribStack(op) => format!("s:{}", op.value()),
        RdlNode::Position(p) => format!(
            "x:{}:{}:{:08x}:{:08x}",
            u8::from(p.rel_h),
            u8::from(p.rel_v),
            p.pos_h.to_bits(),
            p.pos_v.to_bits()
        ),
        RdlNode::Paragraph => "n".to_owned(),
        RdlNode::Justify(j) => format!("j:{}", j.value()),
        RdlNode::Clip(r) => format!("c:{}:{}:{}:{}", r.left, r.top, r.right, r.bottom),
        RdlNode::Style(id) => format!("y:{id}"),
        RdlNode::Null => "0".to_owned(),
        RdlNode::Image(None) => "i:-".to_owned(),
        RdlNode::Image(Some(image)) => format!(
            "i:{:08x}:{:08x}",
            image.width.to_bits(),
            image.height.to_bits()
        ),
        RdlNode::Ref(_) => "ref".to_owned(),
    }
}

fn list_text(list: &RenderDisplayList) -> String {
    list.nodes
        .iter()
        .map(node_text)
        .collect::<Vec<_>>()
        .join(" ")
}

/// The rich fonts suite's lines: each case's INI file (`-` for a missing
/// one) through the `[TrueType]` and `[Style]` readers into a table of
/// unset fonts and an empty registry, then the fonts set and the probed
/// styles (`n<name hex>` through the reader's hash, `i<id>`).
pub fn rich_fonts_lines(input: &[String]) -> Vec<String> {
    use neuron_client::text::font::config::{StandardFont, apply_rich_fonts_ini};
    use neuron_client::text::layout::style::{apply_rich_styles, style_id};

    let unset = StandardFont {
        name: b"\x01unset".to_vec(),
        size: u32::MAX,
        bold: false,
        italic: false,
        underline: false,
    };
    let mut out = Vec::new();
    for (case, line) in input.iter().enumerate() {
        let tokens: Vec<&str> = line.split(' ').filter(|t| !t.is_empty()).collect();
        let contents = match tokens[0] {
            "-" => None,
            file => Some(
                std::fs::read(super::fixtures().join("rich_fonts").join(file))
                    .unwrap_or_else(|e| panic!("{file}: {e}")),
            ),
        };
        let mut fonts = vec![unset.clone(); 32];
        let mut styles = StyleCollection::new();
        if let Some(contents) = &contents {
            apply_rich_fonts_ini(contents, &mut fonts);
            apply_rich_styles(contents, &mut styles);
        }
        let set: Vec<String> = fonts
            .iter()
            .enumerate()
            .filter(|(_, f)| **f != unset)
            .map(|(index, f)| {
                format!(
                    "{index}:{}:{}:{}{}{}",
                    super::hex(&f.name),
                    f.size,
                    u8::from(f.bold),
                    u8::from(f.italic),
                    u8::from(f.underline)
                )
            })
            .collect();
        out.push(format!(
            "{{\"case\":\"{case}\",\"fonts\":\"{}\"}}",
            set.join(" ")
        ));
        for probe in &tokens[1..] {
            let id = match probe.as_bytes()[0] {
                b'n' => style_id(&super::unhex(&probe[1..])),
                _ => probe[1..].parse().expect("a decimal style ID"),
            };
            out.push(format!(
                "{{\"case\":\"{case}\",\"probe\":\"{probe}\",\"id\":\"{id}\",\"nodes\":\"{}\"}}",
                list_text(styles.get(id))
            ));
        }
    }
    out
}

fn layout_case(case: usize, t: &[&str], styles: &StyleCollection) -> String {
    let params = LayoutParams {
        width: t[1].parse().unwrap(),
        origin_h: t[2].parse().unwrap(),
        origin_v: t[3].parse().unwrap(),
        single_line: t[4] == "1",
        wrap: t[5] == "1",
    };
    let attrs = hex_word(t[6]);
    let mut at = 7;
    let source = build(t, &mut at);

    let mut ctx = RecordingTrc::default();
    ctx.begin(attrs);
    let mut laid = RenderDisplayList::new();
    let result = layout_rich_text(&mut ctx, styles, &source, &mut laid, params);
    let layout_log = ctx.take_log();
    let after = ctx.after();

    ctx.reset_to_default();
    ctx.set_pos(0, 0);
    ctx.state.origin_h = 3;
    ctx.state.origin_v = 5;
    laid.execute(&mut ctx, styles, true);
    let exec_log = ctx.take_log();
    let exec_after = ctx.after();

    let (w, h, d) = laid.dimensions(&mut ctx, styles);
    let dims = format!(
        "{:08x}:{:08x}:{:08x}",
        w.to_bits(),
        h.to_bits(),
        d.to_bits()
    );
    let dims_log = ctx.take_log();

    let count = i32::try_from(source.element_count()).unwrap();
    let mut positions = Vec::new();
    for probe in [0, 1, count / 2, count - 1, count] {
        ctx.begin(attrs);
        let (ph, pv, pd) = source.element_pos(&mut ctx, styles, probe);
        positions.push(format!(
            "{probe}={:08x}:{:08x}:{:08x}",
            ph.to_bits(),
            pv.to_bits(),
            pd.to_bits()
        ));
    }
    format!(
        "{{\"case\":\"{case}\",\"result\":\"{}\",\"height\":\"{}\",\"laid\":\"{}\",\"layout\":\"{layout_log}\",\"after\":\"{after}\",\"exec\":\"{exec_log}\",\"exec_after\":\"{exec_after}\",\"dims\":\"{dims}\",\"dims_log\":\"{dims_log}\",\"count\":\"{count}\",\"pos\":\"{}\"}}",
        u8::from(result.broken),
        result.height,
        list_text(&laid),
        positions.join(" ")
    )
}

/// The suite's lines.
pub fn lines(input: &[String]) -> Vec<String> {
    let mut styles = StyleCollection::new();
    let mut out = Vec::new();
    for (case, line) in input.iter().enumerate() {
        let t: Vec<&str> = line.split(' ').filter(|s| !s.is_empty()).collect();
        match t.first().copied() {
            Some("style") => {
                let id: u16 = t[1].parse().unwrap();
                let mut at = 2;
                let list = build(&t, &mut at);
                styles.set_style(id, &list);
                out.push(format!(
                    "{{\"case\":\"{case}\",\"style\":\"{}\",\"nodes\":\"{}\"}}",
                    t[1],
                    list_text(styles.get(id))
                ));
            }
            Some("styleattr") => {
                let id: u16 = t[1].parse().unwrap();
                styles.set_style_attributes(id, hex_word(t[2]));
                out.push(format!(
                    "{{\"case\":\"{case}\",\"style\":\"{}\",\"nodes\":\"{}\"}}",
                    t[1],
                    list_text(styles.get(id))
                ));
            }
            Some("layout") => out.push(layout_case(case, &t, &styles)),
            Some("lex") => {
                let raw = t[1] == "1";
                let split = t.iter().position(|&s| s == "/").unwrap();
                let a = build(&t[2..split], &mut 0);
                let b = build(&t[split + 1..], &mut 0);
                out.push(format!(
                    "{{\"case\":\"{case}\",\"lex\":\"{}\",\"self\":\"{}\"}}",
                    lex_compare(&a, &b, raw),
                    lex_compare(&a, &a, raw)
                ));
            }
            _ => panic!("bad line {line}"),
        }
    }
    out
}
