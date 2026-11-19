//! P4.11 to P4.13: the IME suite (`ime_ui.list`) through the port: the
//! client's IME boxes (`ui::ime`) over a scripted IME context, the boxes'
//! fonts over the deterministic metrics backend, and a recording overlay
//! for the draw. The IME messages the oracle hands `ImeUi_ProcessMessage`
//! go to `ImeBoxes::process`. The format is in
//! `CppSource/Oracle/oracle_ime_ui.cpp`.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_client::text::font::fake::{FakeBackend, FakeLog};
use neuron_client::text::font::quad::ScreenQuadRecord;
use neuron_client::text::font::{FontEnv, FontSystem, LogFont};
use neuron_client::text_input::ime::{CaretRect, ImeContext, ImeView, ime_state};
use neuron_client::text_input::ime_core::{
    CompositionString, ImeFile, ImeMessage, Reading, ReadingText,
};
use neuron_client::text_input::ime_font::{ImeFont, SharedFontBackend};
use neuron_client::text_input::ime_render::{
    Appearance, CandidateList, CandidatePage, LanguageState, parse_appearance_number,
};
use neuron_client::ui::ime::{ClientImeBox, ImeBoxes, ImeOverlay, ImeString, StringRole};
use neuron_client::ui::quad_batch::Frame;
use neuron_client::ui::services::ImeBox;
use neuron_client::ui::tree::{NodeId, Ui};
use neuron_client::ui::widescreen::Surface;
use neuron_client::ui::win3d_renderables::font_atlas_key;

/// What the scripted IME holds, as the oracle's IMM32 stand-ins do.
struct Shared {
    /// The window has its input context.
    open: bool,
    layout: u32,
    ime_open: bool,
    conversion: u32,
    tick: u32,
    blink: u32,
    /// `ImmGetIMEFileNameA`'s name and the version resource (`imefile`).
    file: Option<ImeFile>,
    /// The IME exports `GetReadingString`.
    reading_api: bool,
    /// The reading (`reading`): its units, the error index, vertical.
    reading: Option<(Vec<u16>, u32, bool)>,
    /// The "keyboard mapping" values by key.
    mapping: BTreeMap<String, u8>,
}

impl Default for Shared {
    fn default() -> Self {
        Shared {
            open: true,
            layout: 0x0409_0409,
            ime_open: true,
            conversion: 1,
            tick: 0,
            blink: 500,
            file: None,
            reading_api: false,
            reading: None,
            mapping: BTreeMap::new(),
        }
    }
}

/// The IMEs whose reading records the stand-ins lay out
/// (`ImeUi_GetReadingString`'s cases); 0x05000404's is ANSI.
const RECORD_IDS: [u32; 9] = [
    0x0402_0404,
    0x0403_0404,
    0x0404_0404,
    0x0401_0804,
    0x0402_0804,
    0x0500_0404,
    0x0501_0404,
    0x0502_0404,
    0x0503_0804,
];

/// The window's IME as the stand-ins script it.
struct Scripted(Rc<RefCell<Shared>>);

impl ImeContext for Scripted {
    fn set_open(&mut self, open: bool) {
        self.0.borrow_mut().open = open;
    }
    fn finalize(&mut self, _send: bool) -> Vec<u16> {
        Vec::new()
    }
    fn set_caret(&mut self, _caret: CaretRect) {}
    fn set_boxes_registered(&mut self, _registered: bool) {}
    fn available(&self) -> bool {
        true
    }
    fn view(&mut self) -> ImeView {
        let s = self.0.borrow();
        ImeView {
            state: ime_state(
                s.open,
                true,
                (s.layout & 0xFFFF) as u16,
                s.ime_open,
                s.conversion,
            ),
            language: s.layout,
            tick: s.tick,
            caret_blink: s.blink,
        }
    }
    fn has_context(&mut self) -> bool {
        self.0.borrow().open
    }
    fn layout(&mut self) -> u32 {
        self.0.borrow().layout
    }
    fn ime_file(&mut self) -> Option<ImeFile> {
        self.0.borrow().file.clone()
    }
    fn reading_api(&mut self) -> bool {
        let s = self.0.borrow();
        s.file.is_some() && s.reading_api
    }
    fn reading(&mut self, ime_id: u32, reading_api: bool) -> Option<Reading> {
        let s = self.0.borrow();
        if reading_api {
            let (units, error, vertical) = s.reading.clone().unwrap_or_default();
            return Some(Reading {
                text: ReadingText::Wide(units),
                error,
                vertical,
            });
        }
        // The stand-ins lay the record out for the word the current file
        // and layout make; read with another word's offsets it is empty.
        let (units, error, _) = s.reading.clone()?;
        let ms = s.file.as_ref().and_then(|f| f.version).map_or(0, |v| v.0);
        let laid_out =
            (((ms & 0xFF) << 8) | (ms & 0xFFFF_0000)).wrapping_mul(256) | (s.layout & 0xFFFF);
        if laid_out != ime_id || !RECORD_IDS.contains(&ime_id) {
            return None;
        }
        let text = if ime_id == 0x0500_0404 {
            ReadingText::Ansi(units.iter().map(|&u| u as u8).collect())
        } else {
            ReadingText::Wide(units)
        };
        Some(Reading {
            text,
            error,
            vertical: false,
        })
    }
    fn keyboard_mapping(&mut self, key: &str) -> Option<u8> {
        self.0.borrow().mapping.get(key).copied()
    }
}

fn bits(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

fn units(text: &[u16]) -> String {
    text.iter().map(|u| format!("{u:04x}")).collect()
}

fn decode(hex: &str) -> Vec<u16> {
    if hex == "-" {
        return Vec::new();
    }
    (0..hex.len() / 4)
        .map(|i| u16::from_str_radix(&hex[i * 4..i * 4 + 4], 16).unwrap())
        .collect()
}

fn int(text: &str) -> i32 {
    text.parse().unwrap()
}

fn hex(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap()
}

fn attribute_bytes(text: &str) -> Vec<u8> {
    if text == "-" {
        return Vec::new();
    }
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap())
        .collect()
}

fn composition(f: &[&str]) -> CompositionString {
    CompositionString {
        text: decode(f[0]),
        attributes: attribute_bytes(f[1]),
        cursor: int(f[2]),
    }
}

/// The recording overlay of the draw: the oracle's stand-ins' lines.
struct Recorder<'a> {
    out: &'a mut Vec<String>,
    case: usize,
    fonts: FontSystem,
    atlas_key: u32,
    surface: (i32, i32),
}

impl Recorder<'_> {
    fn ev(&mut self, fields: &str) {
        self.out
            .push(format!("{{\"case\":\"{}\",{fields}}}", self.case));
    }

    fn role(string: &ImeString) -> &'static str {
        match string.role {
            StringRole::Confirm => "confirm",
            StringRole::Text => "text",
            StringRole::Composition => "comp",
        }
    }

    fn text(string: &ImeString) -> Vec<u16> {
        string.child.text().unwrap_or(&[]).to_vec()
    }
}

impl ImeOverlay for Recorder<'_> {
    fn begin(&mut self, primitive: u32) {
        self.ev(&format!("\"ev\":\"begin\",\"p\":\"{primitive}\""));
    }
    fn vertex(&mut self, x: f32, y: f32, z: f32) {
        self.ev(&format!(
            "\"ev\":\"v\",\"x\":\"{}\",\"y\":\"{}\",\"z\":\"{}\"",
            bits(x),
            bits(y),
            bits(z)
        ));
    }
    fn color3ub(&mut self, r: u8, g: u8, b: u8) {
        self.ev(&format!(
            "\"ev\":\"c3\",\"r\":\"{r}\",\"g\":\"{g}\",\"b\":\"{b}\""
        ));
    }
    fn end(&mut self) {
        self.ev("\"ev\":\"end\"");
    }
    fn set_quad_key(&mut self, key: u32) {
        self.ev(&format!("\"ev\":\"qkey\",\"k\":\"{key:08x}\""));
    }
    fn begin_quad_frame(&mut self, frame: Frame, blend_mode: i32, find: bool) {
        self.ev(&format!(
            "\"ev\":\"qframe\",\"f\":\"{:08x} {} {} {} {}\",\"blend\":\"{blend_mode}\",\"find\":\"{}\"",
            frame.texture,
            bits(frame.u0),
            bits(frame.v0),
            bits(frame.u1),
            bits(frame.v1),
            u8::from(find)
        ));
    }
    fn append_quad(&mut self, r: ScreenQuadRecord) {
        self.ev(&format!(
            "\"ev\":\"quad\",\"x0\":\"{}\",\"y0\":\"{}\",\"x1\":\"{}\",\"y1\":\"{}\",\"da\":\"{}\",\"db\":\"{}\",\"u0\":\"{}\",\"v0\":\"{}\",\"u1\":\"{}\",\"v1\":\"{}\",\"c\":\"{:08x}\"",
            bits(r.x0),
            bits(r.y0),
            bits(r.x1),
            bits(r.y1),
            bits(r.depth_a),
            bits(r.depth_b),
            bits(r.u0),
            bits(r.v0),
            bits(r.u1),
            bits(r.v1),
            r.color
        ));
    }
    fn flush_quads(&mut self, key: u32) {
        self.ev(&format!("\"ev\":\"flush\",\"k\":\"{key:08x}\""));
    }
    fn atlas_key(&mut self) -> u32 {
        self.atlas_key
    }
    fn set_atlas_key(&mut self, key: u32) {
        self.atlas_key = key;
        self.ev(&format!("\"ev\":\"akey\",\"k\":\"{key:08x}\""));
    }
    fn set_draw_color(&mut self, packed: u32) {
        self.ev(&format!("\"ev\":\"acolor\",\"c\":\"{packed:08x}\""));
    }
    fn string_text(&mut self, string: &mut ImeString, text: &[u16]) {
        string.child.set_text(&mut self.fonts, Some(text));
    }
    fn string_size(&mut self, string: &mut ImeString) -> (i32, i32) {
        let text = Self::text(string);
        let size = (7 * text.len() as i32, if text.is_empty() { 0 } else { 13 });
        self.ev(&format!(
            "\"ev\":\"ssize\",\"s\":\"{}\",\"t\":\"{}\",\"w\":\"{}\",\"h\":\"{}\"",
            Self::role(string),
            units(&text),
            size.0,
            size.1
        ));
        size
    }
    fn string_paint(&mut self, string: &mut ImeString, x: f32, y: f32) {
        let text = Self::text(string);
        self.ev(&format!(
            "\"ev\":\"spaint\",\"s\":\"{}\",\"t\":\"{}\",\"x\":\"{}\",\"y\":\"{}\"",
            Self::role(string),
            units(&text),
            bits(x),
            bits(y)
        ));
    }
    fn string_release(&mut self, string: &mut ImeString) {
        string.child.release(&mut self.fonts);
    }
    fn surface(&self) -> (i32, i32) {
        self.surface
    }
}

fn appearance_line(name: &str, a: &Appearance) -> String {
    format!(
        "\"ev\":\"appearance\",\"k\":\"{name}\",\"v\":\"{:08x} {:08x} {:08x} {} {} {} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {} {:08x}\"",
        a.symbol_on_color,
        a.symbol_off_color,
        a.symbol_background_color,
        a.symbol_height,
        a.symbol_alpha,
        a.symbol_position,
        a.candidate_text_color,
        a.candidate_selected_text_color,
        a.candidate_color,
        a.composition_input_color,
        a.composition_target_color,
        a.composition_converted_color,
        a.composition_target_converted_color,
        a.composition_error_color,
        a.composition_alpha,
        a.composition_color
    )
}

/// One case's lines.
#[allow(clippy::too_many_lines)]
fn run_case(case: usize, line: &str, out: &mut Vec<String>) {
    let shared = Rc::new(RefCell::new(Shared::default()));
    let (fake, log): (FakeBackend, Rc<RefCell<FakeLog>>) = FakeBackend::with_log();
    let mut backend = Some(SharedFontBackend::new(Box::new(fake)));
    let mut log_font = LogFont::new(b"Arial", 16, false, false, false);
    // The focus manager comes with "focus" (`new ClientIMEFocus`).
    let mut hub: Option<ImeBoxes> = None;
    let mut ui = Ui::new(Surface::new(800, 600));
    let mut owners: Vec<NodeId> = Vec::new();
    let mut boxes: Vec<Option<ClientImeBox>> = Vec::new();
    let mut surface = (800, 600);
    let mut recorder_fonts = Some(FontSystem::new(
        Box::new(FakeBackend::default()),
        FontEnv::new(800, 600),
    ));
    let mut atlas_key = font_atlas_key();
    let ev = |out: &mut Vec<String>, fields: String| {
        out.push(format!("{{\"case\":\"{case}\",{fields}}}"));
    };
    for op in line.split(" ; ") {
        let f: Vec<&str> = op.split(' ').collect();
        let name = f[0];
        let mut texts: BTreeMap<usize, Vec<u16>> = BTreeMap::new();
        let owner_index = |owners: &[NodeId], node: NodeId| owners.iter().position(|&o| o == node);
        let needs_box = matches!(
            name,
            "del"
                | "key"
                | "mb"
                | "wc"
                | "resize"
                | "force"
                | "caret"
                | "fheight"
                | "extent"
                | "state"
        );
        let index = if needs_box { int(f[1]) as usize } else { 0 };
        if needs_box && boxes.get(index).is_none_or(Option::is_none) {
            continue;
        }
        let process = |message: ImeMessage| {
            if let Some(hub) = &hub {
                hub.process(message);
            }
        };
        let direct = |texts: &mut BTreeMap<usize, Vec<u16>>, text: Option<Vec<u16>>| {
            if let Some(t) = text {
                texts.insert(index, t);
            }
        };
        match name {
            "screen" => surface = (int(f[1]), int(f[2])),
            "blink" => shared.borrow_mut().blink = int(f[1]) as u32,
            "lf" => {
                let face: Vec<u8> = decode(f[2]).iter().map(|&u| u as u8).collect();
                log_font = LogFont::new(&face, int(f[1]), false, false, false);
                if let Some(hub) = &hub {
                    hub.set_confirm_font(None, Some(log_font));
                }
            }
            "focus" => {
                let made = ImeBoxes::new(
                    Box::new(Scripted(Rc::clone(&shared))),
                    backend.take().unwrap(),
                );
                made.set_confirm_font(None, Some(log_font));
                hub = Some(made);
                ev(out, "\"ev\":\"focus\",\"avail\":\"1\"".to_owned());
            }
            "box" => {
                let Some(hub) = &hub else { continue };
                let node = ui.alloc(None);
                owners.push(node);
                boxes.push(Some(hub.create_client_box(node)));
            }
            "del" => boxes[index] = None,
            "key" => {
                let b = boxes[index].as_mut().unwrap();
                let (r, text) = b.edit(|e| e.handle_key(int(f[2]) as u32));
                ev(
                    out,
                    format!("\"ev\":\"r\",\"v\":\"{}\"", u8::from(r.unwrap_or(false))),
                );
                direct(&mut texts, text);
            }
            "mb" | "wc" => {
                let unit = if name == "mb" {
                    int(f[2]) as u16
                } else {
                    u16::from_str_radix(f[2], 16).unwrap()
                };
                let b = boxes[index].as_mut().unwrap();
                let (r, text) = b.edit(|e| e.handle_wide_char(unit));
                ev(
                    out,
                    format!("\"ev\":\"r\",\"v\":\"{}\"", u8::from(r.unwrap_or(false))),
                );
                direct(&mut texts, text);
            }
            "onchar" => {
                let Some(hub) = &hub else { continue };
                let r = hub.on_char(u16::from_str_radix(f[1], 16).unwrap());
                ev(out, format!("\"ev\":\"r\",\"v\":\"{}\"", u8::from(r)));
            }
            "onkey" => {
                let Some(hub) = &hub else { continue };
                let r = hub.on_key_down(int(f[1]) as u32);
                ev(out, format!("\"ev\":\"r\",\"v\":\"{}\"", u8::from(r)));
            }
            "resize" => {
                let b = boxes[index].as_mut().unwrap();
                b.resize(
                    int(f[2]) as u32,
                    [int(f[3]), int(f[4]), int(f[5]), int(f[6])],
                );
            }
            "setfocus" => {
                let Some(hub) = &hub else { continue };
                if f[1] == "-" {
                    hub.set_focus(None, false);
                } else if let Some(Some(b)) = boxes.get(int(f[1]) as usize) {
                    hub.set_focus(Some(b.id()), false);
                } else {
                    hub.set_focus(None, false);
                }
            }
            "force" => {
                let Some(hub) = &hub else { continue };
                hub.set_focus(Some(boxes[index].as_ref().unwrap().id()), true);
            }
            "win" => {
                let Some(hub) = &hub else { continue };
                hub.on_window_focus(int(f[1]) != 0);
            }
            "lang" => {
                shared.borrow_mut().layout = hex(f[1]);
                process(ImeMessage::StateChanged);
            }
            "langchange" => {
                shared.borrow_mut().layout = hex(f[1]);
                process(ImeMessage::InputLanguageChanged);
            }
            "imestate" => {
                {
                    let mut s = shared.borrow_mut();
                    s.ime_open = int(f[1]) != 0;
                    s.conversion = int(f[2]) as u32;
                }
                process(ImeMessage::StateChanged);
            }
            "imefile" => {
                let mut s = shared.borrow_mut();
                s.file = (f[1] != "-").then(|| ImeFile {
                    name: f[1].to_owned(),
                    version: (f[2] != "-").then(|| (hex(f[2]), hex(f[3]))),
                });
                s.reading_api = int(f[4]) != 0;
            }
            "kbmap" => {
                let mut s = shared.borrow_mut();
                if f[2] == "-" {
                    s.mapping.remove(f[1]);
                } else {
                    s.mapping.insert(f[1].to_owned(), hex(f[2]) as u8);
                }
            }
            "reading" => {
                shared.borrow_mut().reading =
                    Some((decode(f[1]), int(f[2]) as u32, int(f[3]) != 0));
            }
            "level" => {
                if let Some(hub) = &hub {
                    hub.set_support_level(int(f[1]) as u32);
                }
            }
            "startcomp" => process(ImeMessage::StartComposition),
            "comp" => process(ImeMessage::Composition {
                result: None,
                composition: Some(composition(&f[1..4])),
            }),
            "result" => process(ImeMessage::Composition {
                result: Some(decode(f[1])),
                composition: None,
            }),
            "resultcomp" => process(ImeMessage::Composition {
                result: Some(decode(f[1])),
                composition: Some(composition(&f[2..5])),
            }),
            "endcomp" => process(ImeMessage::EndComposition),
            "cand" => process(ImeMessage::OpenCandidate(Some(CandidateList {
                selection: int(f[1]) as u32,
                page_size: int(f[2]) as u32,
                strings: f[3..].iter().map(|t| decode(t)).collect(),
            }))),
            "nocand" => process(ImeMessage::CloseCandidate),
            "private" => process(ImeMessage::Private(hex(f[1]))),
            "tick" => shared.borrow_mut().tick = f[1].parse().unwrap(),
            "caret" => {
                let Some(hub) = &hub else { continue };
                let id = boxes[index].as_ref().unwrap().id();
                let (tick, blink) = {
                    let s = shared.borrow();
                    (s.tick, s.blink)
                };
                let empty = hub.composition_empty();
                let r = hub
                    .with_box_mut(id, |b| b.caret_mut().update(tick, blink, empty))
                    .unwrap_or(false);
                ev(out, format!("\"ev\":\"caret\",\"v\":\"{}\"", u8::from(r)));
            }
            "fheight" => {
                let Some(hub) = &hub else { continue };
                let id = boxes[index].as_ref().unwrap().id();
                hub.font(id).unwrap().set_height(int(f[2]));
            }
            "extent" => {
                let Some(hub) = &hub else { continue };
                let id = boxes[index].as_ref().unwrap().id();
                let (w, h) = hub.font(id).unwrap().text_extent(&decode(f[2]));
                ev(
                    out,
                    format!("\"ev\":\"extent\",\"w\":\"{w}\",\"h\":\"{h}\""),
                );
            }
            "draw" => {
                if let Some(hub) = &hub {
                    let mut recorder = Recorder {
                        out: &mut *out,
                        case,
                        fonts: recorder_fonts.take().unwrap(),
                        atlas_key,
                        surface,
                    };
                    hub.draw(&mut recorder);
                    atlas_key = recorder.atlas_key;
                    recorder_fonts = Some(recorder.fonts);
                }
            }
            "state" => {
                let Some(hub) = &hub else { continue };
                let id = boxes[index].as_ref().unwrap().id();
                let line = hub
                    .with_box(id, |b| {
                        let r = b.rect();
                        let (px, py) = b.caret().position();
                        let (cx, cy) = b.caret_offset();
                        let (mx, my) = b.margins();
                        format!(
                            "\"ev\":\"box\",\"text\":\"{}\",\"buf\":\"{}\",\"size\":\"{}\",\"caret\":\"{}\",\"cx\":\"{cx}\",\"cy\":\"{cy}\",\"px\":\"{px}\",\"py\":\"{py}\",\"shown\":\"{}\",\"blink\":\"{}\",\"focus\":\"{}\",\"insert\":\"{}\",\"rect\":\"{} {} {} {}\",\"margins\":\"{mx} {my}\"",
                            units(b.text()),
                            units(b.buffer()),
                            b.buffer_size(),
                            b.caret_index(),
                            u8::from(b.caret().is_shown()),
                            u8::from(b.caret().blinks()),
                            u8::from(b.has_focus()),
                            u8::from(b.insert_mode()),
                            r.left,
                            r.top,
                            r.right,
                            r.bottom
                        )
                    })
                    .unwrap();
                ev(out, line);
            }
            "fstate" => {
                let focus = hub.as_ref().and_then(ImeBoxes::focus).and_then(|id| {
                    boxes
                        .iter()
                        .position(|b| b.as_ref().is_some_and(|b| b.id() == id))
                });
                ev(
                    out,
                    format!(
                        "\"ev\":\"fstate\",\"n\":\"{}\",\"focus\":\"{}\"",
                        hub.as_ref().map_or(0, ImeBoxes::len),
                        focus.map_or(-1, |i| i as i32)
                    ),
                );
            }
            "gdi" => {
                let l = log.borrow();
                let created: Vec<String> = l.created.iter().map(i32::to_string).collect();
                ev(
                    out,
                    format!(
                        "\"ev\":\"gdi\",\"created\":\"{}\",\"deleted\":\"{}\"",
                        created.join(" "),
                        l.deleted
                    ),
                );
            }
            "appearance" => {
                let a = Appearance::default_strings();
                ev(out, appearance_line("default", &a));
                ev(out, appearance_line("set", &a.set()));
            }
            "parse" => {
                let v = parse_appearance_number(&decode(f[1]));
                ev(out, format!("\"ev\":\"parse\",\"v\":\"{v:08x}\""));
            }
            "langstate" => {
                let language = hub
                    .as_ref()
                    .map_or_else(LanguageState::default, ImeBoxes::language);
                ev(
                    out,
                    format!(
                        "\"ev\":\"lang\",\"vertical\":\"{}\",\"symbol\":\"{}\"",
                        u8::from(language.vertical),
                        units(&language.symbol)
                    ),
                );
            }
            "candstate" => {
                let page = hub
                    .as_ref()
                    .map_or_else(CandidatePage::default, ImeBoxes::candidates);
                let entries: Vec<String> = (0..10)
                    .map(|i| {
                        let e = page.entry(i);
                        if e.is_empty() {
                            "-".to_owned()
                        } else {
                            units(e)
                        }
                    })
                    .collect();
                ev(
                    out,
                    format!(
                        "\"ev\":\"cand\",\"count\":\"{}\",\"page\":\"{}\",\"sel\":\"{}\",\"e\":\"{}\"",
                        page.count,
                        page.page_size,
                        page.selection,
                        entries.join(" ")
                    ),
                );
            }
            "readstate" => {
                let page = hub
                    .as_ref()
                    .map_or_else(CandidatePage::default, ImeBoxes::candidates);
                let reading = &page.reading[..page
                    .reading
                    .iter()
                    .position(|&u| u == 0)
                    .unwrap_or(page.reading.len())];
                ev(
                    out,
                    format!(
                        "\"ev\":\"read\",\"show\":\"{}\",\"horiz\":\"{}\",\"reading\":\"{}\",\"sel\":\"{}\"",
                        u8::from(page.show_reading),
                        u8::from(page.horizontal_reading),
                        units(reading),
                        page.reading_selection
                    ),
                );
            }
            "imeid" => {
                // GetImeId(1) first, as the oracle asks.
                let (v1, v0) = hub.as_ref().map_or((0, 0), |h| (h.ime_id(1), h.ime_id(0)));
                ev(
                    out,
                    format!("\"ev\":\"imeid\",\"v0\":\"{v0:08x}\",\"v1\":\"{v1:08x}\""),
                );
            }
            "compstate" => {
                let (text, caret, cursor, level, insert) =
                    hub.as_ref().map_or((Vec::new(), 0, 0, 0, true), |h| {
                        let (t, c, k) = h.composition();
                        (t, c, k, h.support_level(), h.insert_mode())
                    });
                ev(
                    out,
                    format!(
                        "\"ev\":\"comp\",\"t\":\"{}\",\"caret\":\"{caret}\",\"cursor\":\"{cursor}\",\"level\":\"{level}\",\"insert\":\"{}\"",
                        units(&text),
                        u8::from(insert)
                    ),
                );
            }
            other => panic!("unknown operation {other}"),
        }
        if let Some(hub) = &hub {
            for (owner, text) in hub.take_owner_texts() {
                if let Some(i) = owner_index(&owners, owner) {
                    texts.insert(i, text);
                }
            }
        }
        for (i, text) in texts {
            ev(
                out,
                format!(
                    "\"ev\":\"settext\",\"o\":\"{i}\",\"t\":\"{}\"",
                    units(&text)
                ),
            );
        }
    }
}

/// The suite's lines.
pub fn lines(input: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input.iter().enumerate() {
        run_case(case, line, &mut out);
    }
    out
}
