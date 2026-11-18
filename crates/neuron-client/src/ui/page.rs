//! Page descriptors and pages: `ui_page_desc.cpp` (`FUiPageDesc`, the
//! loader of the `PAGE_*.page` files of the front end) and `ui_page.cpp`
//! (`UIPage::Page`, `UIPage::MasterWin`).
//!
//! A page file is text, one widget per line: the class token, the
//! instance name, four floats (left, top, width, height; `#` lines and
//! lines starting with white space are skipped) and what the class wants
//! after them, read with `sscanf` formats chosen by the token. Any unknown
//! token or field count ends the load as a failure. A page is made by a
//! `MasterWin`: it unprojects the surface's corners through the Win3D
//! camera, and places each entry at (left, top) in units of the visible
//! height on the near plane, through a frame that scales by that height.
//! The 4:3 layout thus fills the height of the surface and, with the Hor+
//! camera, sits in the middle of a wider one.

use std::collections::BTreeMap;

use neuron_core::math::{Matrix, Transform, Vector, mul_transform, transform_matrix};
use neuron_core::num::trunc_to_i32;

use super::tree::{DrawHost, NodeId, Payload, Ui, Widget};

/// `kUiPageLoadMagicBits`: the aspect a successful load writes (4:3).
pub const LOAD_MAGIC_BITS: u32 = 0x3FAA_A993;

/// The depth bias of the page frame (bits 0xB22BCC77, -1e-8).
pub const DEPTH_BIAS: f32 = f32::from_bits(0xB22B_CC77);

/// The style name of a `UITextMsgButton`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Style {
    /// `"<none>"`: null.
    None,
    /// `""` or `"<default>"`: the shared `kUiPageDefaultStyle`.
    Default,
    Named(Vec<u8>),
}

/// The class payloads (`FUiPayload*`).
#[derive(Clone, Debug, PartialEq)]
pub enum EntryPayload {
    /// `UIStaticText`, `UITextEdit`: the caption, each byte sign-extended to
    /// a UTF-16 unit.
    Text {
        right: f32,
        bottom: f32,
        caption: Vec<u16>,
    },
    /// `UITextMsgButton`.
    TextMsgButton {
        right: f32,
        bottom: f32,
        caption: Vec<u16>,
        /// The float after the caption, truncated (the previous line's
        /// value when the field is missing).
        extra_int: i32,
        /// The trailing integer, 0 when missing.
        extra_id: i32,
        style: Style,
    },
    /// `UIListBox`, `UIWinDeform`, `UIRichTextWin`, `UIShipViewControl`.
    Bounds { right: f32, bottom: f32 },
    /// `UIWinObject`: a mesh path and a fifth float.
    WinObject {
        right: f32,
        bottom: f32,
        extra_name: Vec<u8>,
        extra_float: f32,
    },
    /// `UITable`, `UIFastTable`: the fifth and sixth floats truncated.
    Table {
        right: f32,
        bottom: f32,
        int5: i32,
        int6: i32,
    },
    /// `UIScroll`, `UICycle`: four more floats and a flag.
    ScrollCycle {
        right: f32,
        bottom: f32,
        extra: [f32; 4],
        flag: bool,
    },
}

/// `FUiWidgetEntry`.
#[derive(Clone, Debug, PartialEq)]
pub struct WidgetEntry {
    pub type_name: Vec<u8>,
    pub name: Vec<u8>,
    pub left: f32,
    pub top: f32,
    pub right: f32,
    pub bottom: f32,
    pub payload: EntryPayload,
}

/// A loaded page descriptor: `FUiPageDesc` with its `FUiPageData`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PageDesc {
    /// The entries read, also those before a failure.
    pub entries: Vec<WidgetEntry>,
    /// Whether the load succeeded (the page data has entries only then).
    pub ok: bool,
    /// `FUiPageData::Aspect`: [`LOAD_MAGIC_BITS`] after a success, else 0.
    pub aspect: f32,
}

/// A cursor over one line, with the `sscanf` directives the loader uses.
struct Scan<'a> {
    text: &'a [u8],
    at: usize,
}

/// `isspace` in the "C" locale.
fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r')
}

impl<'a> Scan<'a> {
    fn new(text: &'a [u8]) -> Self {
        Scan { text, at: 0 }
    }

    fn skip_space(&mut self) {
        while self.at < self.text.len() && is_space(self.text[self.at]) {
            self.at += 1;
        }
    }

    /// `%s`.
    fn string(&mut self) -> Option<Vec<u8>> {
        self.skip_space();
        let start = self.at;
        while self.at < self.text.len() && !is_space(self.text[self.at]) {
            self.at += 1;
        }
        (self.at > start).then(|| self.text[start..self.at].to_vec())
    }

    /// `%f`: a decimal number with an optional sign, fraction and
    /// exponent, rounded to `float` (the forms the scanner shares with
    /// `strtod` beyond these, `inf`, `nan` and hexadecimal, are not
    /// accepted: divergence D344).
    fn float(&mut self) -> Option<f32> {
        self.skip_space();
        let t = self.text;
        let start = self.at;
        let mut i = self.at;
        if i < t.len() && (t[i] == b'+' || t[i] == b'-') {
            i += 1;
        }
        let mut digits = 0;
        while i < t.len() && t[i].is_ascii_digit() {
            i += 1;
            digits += 1;
        }
        if i < t.len() && t[i] == b'.' {
            i += 1;
            while i < t.len() && t[i].is_ascii_digit() {
                i += 1;
                digits += 1;
            }
        }
        if digits == 0 {
            return None;
        }
        if i < t.len() && (t[i] == b'e' || t[i] == b'E') {
            let mut j = i + 1;
            if j < t.len() && (t[j] == b'+' || t[j] == b'-') {
                j += 1;
            }
            let exp_start = j;
            while j < t.len() && t[j].is_ascii_digit() {
                j += 1;
            }
            if j > exp_start {
                i = j;
            }
        }
        self.at = i;
        let text = std::str::from_utf8(&t[start..i]).ok()?;
        let text = text.strip_prefix('+').unwrap_or(text);
        // A lone "." with digits around was counted above; Rust's parser
        // wants a digit next to the point, which `digits > 0` ensures.
        let normalized;
        let text = if text.starts_with('.') || text.starts_with("-.") {
            normalized = text.replacen('.', "0.", 1);
            normalized.as_str()
        } else {
            text
        };
        text.parse::<f32>().ok()
    }

    /// `%d`.
    fn int(&mut self) -> Option<i32> {
        self.skip_space();
        let t = self.text;
        let start = self.at;
        let mut i = self.at;
        let negative = i < t.len() && t[i] == b'-';
        if i < t.len() && (t[i] == b'+' || t[i] == b'-') {
            i += 1;
        }
        let digits_start = i;
        let mut value: i64 = 0;
        while i < t.len() && t[i].is_ascii_digit() {
            value = value.wrapping_mul(10).wrapping_add(i64::from(t[i] - b'0'));
            i += 1;
        }
        if i == digits_start {
            self.at = start;
            return None;
        }
        self.at = i;
        let value = if negative {
            value.wrapping_neg()
        } else {
            value
        };
        Some(value as i32)
    }

    /// A literal character after optional white space (the format's ` \"`).
    fn literal(&mut self, c: u8) -> bool {
        self.skip_space();
        if self.at < self.text.len() && self.text[self.at] == c {
            self.at += 1;
            true
        } else {
            false
        }
    }

    /// `%[^"]`: one or more bytes other than `"`.
    fn not_quote(&mut self) -> Option<Vec<u8>> {
        let start = self.at;
        while self.at < self.text.len() && self.text[self.at] != b'"' {
            self.at += 1;
        }
        (self.at > start).then(|| self.text[start..self.at].to_vec())
    }
}

/// `TrackedWiden`: each byte sign-extended into a 16-bit unit.
fn widen(bytes: &[u8]) -> Vec<u16> {
    bytes
        .iter()
        .map(|&b| i16::from(b.cast_signed()).cast_unsigned())
        .collect()
}

/// The head every format starts with: type, name and up to four floats;
/// the count of fields read.
fn head(scan: &mut Scan<'_>) -> (usize, Vec<u8>, Vec<u8>, [f32; 4]) {
    let mut floats = [0.0f32; 4];
    let Some(ty) = scan.string() else {
        return (0, Vec::new(), Vec::new(), floats);
    };
    let Some(name) = scan.string() else {
        return (1, ty, Vec::new(), floats);
    };
    let mut count = 2;
    for f in &mut floats {
        match scan.float() {
            Some(v) => {
                *f = v;
                count += 1;
            }
            None => break,
        }
    }
    (count, ty, name, floats)
}

/// `"%s %s %f %f %f %f \"%[^\"]"`: the head and an optional caption.
fn head_caption(scan: &mut Scan<'_>) -> (usize, Vec<u8>, Vec<u8>, [f32; 4], Vec<u8>) {
    let (mut count, ty, name, f) = head(scan);
    let mut caption = Vec::new();
    if count == 6
        && scan.literal(b'"')
        && let Some(c) = scan.not_quote()
    {
        caption = c;
        count += 1;
    }
    (count, ty, name, f, caption)
}

/// Floats after the head, while they read.
fn more_floats(scan: &mut Scan<'_>, out: &mut [f32]) -> usize {
    let mut n = 0;
    for f in out {
        match scan.float() {
            Some(v) => {
                *f = v;
                n += 1;
            }
            None => break,
        }
    }
    n
}

fn entry(ty: Vec<u8>, name: Vec<u8>, f: [f32; 4], payload: EntryPayload) -> WidgetEntry {
    WidgetEntry {
        type_name: ty,
        name,
        left: f[0],
        top: f[1],
        right: f[2],
        bottom: f[3],
        payload,
    }
}

/// The lines `fgets` with a 0x100-byte buffer gives from a file opened in
/// text mode: CR LF read as LF, input ending at a Ctrl-Z, a line longer
/// than 255 bytes split.
fn text_lines(bytes: &[u8]) -> Vec<Vec<u8>> {
    let mut text = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        if b == 0x1A {
            break;
        }
        if b == b'\r' && bytes.get(i + 1) == Some(&b'\n') {
            i += 1;
            continue;
        }
        text.push(b);
        i += 1;
    }
    let mut lines = Vec::new();
    let mut current = Vec::new();
    for b in text {
        current.push(b);
        if b == b'\n' || current.len() == 0xFF {
            lines.push(std::mem::take(&mut current));
        }
    }
    if !current.is_empty() {
        lines.push(current);
    }
    lines
}

impl PageDesc {
    /// `FUiPageDesc::LoadFromFile` over the file's bytes.
    #[must_use]
    pub fn load(bytes: &[u8]) -> PageDesc {
        let mut desc = PageDesc {
            entries: Vec::new(),
            ok: true,
            aspect: 0.0,
        };
        // Not reset per line in the reference.
        let mut msg_extra_float = 0.0f32;
        for line in text_lines(bytes) {
            let line = line.as_slice();
            if line[0] == b'#' || is_space(line[0]) {
                continue;
            }
            let Some(ty) = Scan::new(line).string() else {
                continue;
            };
            let mut scan = Scan::new(line);
            let parsed = match ty.as_slice() {
                b"UIStaticText" | b"UITextEdit" => {
                    let (count, ty, name, f, caption) = head_caption(&mut scan);
                    (count == 6 || count == 7).then(|| {
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::Text {
                                right: f[2],
                                bottom: f[3],
                                caption: widen(&caption),
                            },
                        )
                    })
                }
                b"UITextMsgButton" => {
                    let (mut count, ty, name, f, caption) = head_caption(&mut scan);
                    let mut style = Vec::new();
                    let mut extra_id = 0;
                    if count == 7
                        && scan.literal(b'"')
                        && let Some(v) = scan.float()
                    {
                        msg_extra_float = v;
                        count += 1;
                        if scan.literal(b'"')
                            && let Some(s) = scan.not_quote()
                        {
                            style = s;
                            count += 1;
                            if scan.literal(b'"')
                                && let Some(id) = scan.int()
                            {
                                extra_id = id;
                                count += 1;
                            }
                        }
                    }
                    (7..=10).contains(&count).then(|| {
                        let style = if style == b"<none>" {
                            Style::None
                        } else if style.is_empty() || style == b"<default>" {
                            Style::Default
                        } else {
                            Style::Named(style)
                        };
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::TextMsgButton {
                                right: f[2],
                                bottom: f[3],
                                caption: widen(&caption),
                                extra_int: trunc_to_i32(f64::from(msg_extra_float)),
                                extra_id,
                                style,
                            },
                        )
                    })
                }
                b"UIListBox" | b"UIWinDeform" | b"UIRichTextWin" | b"UIShipViewControl" => {
                    let (count, ty, name, f) = head(&mut scan);
                    (count == 6).then(|| {
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::Bounds {
                                right: f[2],
                                bottom: f[3],
                            },
                        )
                    })
                }
                b"UIWinObject" => {
                    let (mut count, ty, name, f) = head(&mut scan);
                    let mut f5 = [0.0f32];
                    let mut extra_name = Vec::new();
                    if count == 6 && more_floats(&mut scan, &mut f5) == 1 {
                        count += 1;
                        if let Some(s) = scan.string() {
                            extra_name = s;
                            count += 1;
                        }
                    }
                    (count == 8).then(|| {
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::WinObject {
                                right: f[2],
                                bottom: f[3],
                                extra_name,
                                extra_float: f5[0],
                            },
                        )
                    })
                }
                b"UITable" | b"UIFastTable" => {
                    let (mut count, ty, name, f) = head(&mut scan);
                    let mut extra = [0.0f32; 2];
                    if count == 6 {
                        count += more_floats(&mut scan, &mut extra);
                    }
                    (count == 8).then(|| {
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::Table {
                                right: f[2],
                                bottom: f[3],
                                int5: trunc_to_i32(f64::from(extra[0])),
                                int6: trunc_to_i32(f64::from(extra[1])),
                            },
                        )
                    })
                }
                b"UIScroll" | b"UICycle" => {
                    let (mut count, ty, name, f) = head(&mut scan);
                    let mut extra = [0.0f32; 4];
                    let mut flag = 0;
                    if count == 6 {
                        let n = more_floats(&mut scan, &mut extra);
                        count += n;
                        if n == 4
                            && let Some(v) = scan.int()
                        {
                            flag = v;
                            count += 1;
                        }
                    }
                    (count == 10 || count == 11).then(|| {
                        entry(
                            ty,
                            name,
                            f,
                            EntryPayload::ScrollCycle {
                                right: f[2],
                                bottom: f[3],
                                extra,
                                flag: flag != 0,
                            },
                        )
                    })
                }
                _ => None,
            };
            match parsed {
                Some(e) => desc.entries.push(e),
                None => {
                    desc.ok = false;
                    break;
                }
            }
        }
        if desc.ok {
            desc.aspect = f32::from_bits(LOAD_MAGIC_BITS);
        }
        desc
    }

    /// The page data's entries: all of them after a success, none after a
    /// failure (`FUiPageData::Entries` is null then).
    #[must_use]
    pub fn data_entries(&self) -> &[WidgetEntry] {
        if self.ok { &self.entries } else { &[] }
    }
}

/// `UIPage::Page`: the widgets of a page by instance name.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Page {
    /// 1.0 until made, then the descriptor's aspect.
    pub aspect: f32,
    widgets: BTreeMap<Vec<u8>, NodeId>,
}

impl Page {
    /// `GetCount`.
    #[must_use]
    pub fn count(&self) -> usize {
        self.widgets.len()
    }

    /// `GetWidgetName`: the index-th name in map order.
    #[must_use]
    pub fn widget_name(&self, index: usize) -> Option<&[u8]> {
        self.widgets.keys().nth(index).map(Vec::as_slice)
    }

    /// `GetWidget`.
    #[must_use]
    pub fn widget(&self, index: usize) -> Option<NodeId> {
        self.widgets.values().nth(index).copied()
    }

    /// `FindWidget`.
    #[must_use]
    pub fn find(&self, name: &[u8]) -> Option<NodeId> {
        self.widgets.get(name).copied()
    }

    /// `Instantiate`: each entry made through the factory under `parent`,
    /// placed at `frame * (left, top, 0)` (slot 8) and recorded by name (a
    /// second entry of a name replaces the first in the map). False at the
    /// first factory miss.
    pub fn instantiate(
        &mut self,
        ui: &mut Ui,
        entries: &[WidgetEntry],
        parent: Option<NodeId>,
        frame: &Transform,
    ) -> bool {
        for e in entries {
            let Some(widget) = ui.create_by_class(
                Some(&e.name),
                &e.type_name,
                parent,
                &mut Payload::Page(&e.payload),
            ) else {
                return false;
            };
            let local = Transform::new(Matrix::IDENTITY, Vector::new(e.left, e.top, 0.0));
            let placed = mul_transform(frame, &local);
            ui.set_transform(widget, &placed);
            self.widgets.insert(e.name.clone(), widget);
        }
        true
    }

    /// `CreatePage`: a page of `entries`; on failure the widgets made so
    /// far are deleted and `None` returned.
    pub fn create(
        ui: &mut Ui,
        aspect: f32,
        entries: &[WidgetEntry],
        frame: &Transform,
        parent: Option<NodeId>,
    ) -> Option<Page> {
        let mut page = Page {
            aspect,
            widgets: BTreeMap::new(),
        };
        if page.instantiate(ui, entries, parent, frame) {
            Some(page)
        } else {
            page.delete(ui);
            None
        }
    }

    /// `~Page`: every mapped widget deleted.
    pub fn delete(&mut self, ui: &mut Ui) {
        for (_, id) in std::mem::take(&mut self.widgets) {
            ui.delete(id);
        }
    }
}

/// The page frame of `MasterWin::Create`: the surface corners unprojected,
/// a scale of the visible height on x and y, the near plane's depth less
/// 1e-8.
#[must_use]
pub fn page_frame(ui: &Ui) -> Transform {
    let camera = &ui.camera;
    let top_left = camera.unproject(0.0, 0.0);
    let bottom_right = camera.unproject(camera.surface.width as f32, camera.surface.height as f32);
    let height = (f64::from(top_left.y) - f64::from(bottom_right.y)) as f32;
    Transform::new(
        Matrix::from_rows([[height, 0.0, 0.0], [0.0, height, 0.0], [0.0, 0.0, 1.0]]),
        Vector::new(
            0.0,
            0.0,
            (f64::from(DEPTH_BIAS) + f64::from(top_left.z)) as f32,
        ),
    )
}

/// `UiWidgetProjectExtent`: a widget's width and height through its
/// orientation at depth `z`, as the `UI*` controls' slot 8 computes them.
#[must_use]
pub fn project_extent(matrix: &Matrix, z: f32, width: f32, height: f32) -> (f32, f32) {
    let width_point = transform_matrix(matrix, Vector::new(width, 0.0, z));
    let origin = transform_matrix(matrix, Vector::new(0.0, 0.0, z));
    let out_width = ((f64::from(width_point.x) - f64::from(origin.x)) as f32).abs();
    let height_point = transform_matrix(matrix, Vector::new(0.0, height, z));
    let origin = transform_matrix(matrix, Vector::new(0.0, 0.0, z));
    let out_height = ((f64::from(height_point.y) - f64::from(origin.y)) as f32).abs();
    (out_width, out_height)
}

/// `UIPage::MasterWin`: the root of a page screen.
#[derive(Debug, Default)]
pub struct MasterWin {
    /// The page data: the descriptor's aspect and entries.
    pub aspect: f32,
    pub entries: Vec<WidgetEntry>,
    pub page: Option<Page>,
}

impl MasterWin {
    /// `MasterWin(FUiPageData*)` over a loaded descriptor.
    #[must_use]
    pub fn new(desc: &PageDesc) -> MasterWin {
        MasterWin {
            aspect: desc.aspect,
            entries: desc.data_entries().to_vec(),
            page: None,
        }
    }

    /// Makes a master window as a root (`new MasterWin`, `Create(0, 0)`).
    pub fn open(ui: &mut Ui, desc: &PageDesc) -> Option<NodeId> {
        ui.create(
            Box::new(MasterWin::new(desc)),
            None,
            None,
            &mut Payload::None,
        )
    }
}

impl Widget for MasterWin {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }

    /// `MasterWin::Create`: the `Win3D` part, then the page in the page
    /// frame (false without page data or when the page fails).
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        if self.entries.is_empty() && self.aspect == 0.0 {
            return false;
        }
        let frame = page_frame(ui);
        self.page = Page::create(ui, self.aspect, &self.entries, &frame, Some(id));
        self.page.is_some()
    }

    /// `MasterWin::Destroy`: the page deleted, then the `Win3D` part.
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(mut page) = self.page.take() {
            page.delete(ui);
        }
        ui.base_destroy(id);
    }

    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(mut page) = self.page.take() {
            page.delete(ui);
        }
        ui.base_on_delete(id);
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_shipped_style_page_loads() {
        let text = b"#Automatically generated. Do not edit!\r\n\
UIWinObject Box -.6 -.3  .88  .17 -1.0000 interface\\intro\\objects\\box.cmp \r\n\
UIFastTable Table -.58  .22  1.17  .39 9.0000 8.0000 \r\n\
UITextMsgButton Back -.58 -.24  .3125  6.51041666666667E-02 \"Back\" 1001.0000 \"<default>\"  393251 \r\n\
UIScroll Scroll  .6  .2  2.27E-02  .36 0.0000 100.0000 14.0000 1.0000  0 \r\n\
UIRichTextWin Details -.62 -.34  .83  .11 \r\n\
UIStaticText Caption 0 0 1 1 \"Hello\" \r\n";
        let desc = PageDesc::load(text);
        assert!(desc.ok);
        assert_eq!(desc.aspect.to_bits(), LOAD_MAGIC_BITS);
        assert_eq!(desc.entries.len(), 6);
        assert_eq!(desc.entries[0].left, -0.6);
        assert_eq!(
            desc.entries[0].payload,
            EntryPayload::WinObject {
                right: 0.88,
                bottom: 0.17,
                extra_name: b"interface\\intro\\objects\\box.cmp".to_vec(),
                extra_float: -1.0,
            }
        );
        assert_eq!(
            desc.entries[1].payload,
            EntryPayload::Table {
                right: 1.17,
                bottom: 0.39,
                int5: 9,
                int6: 8
            }
        );
        let EntryPayload::TextMsgButton {
            caption,
            extra_int,
            extra_id,
            style,
            bottom,
            ..
        } = &desc.entries[2].payload
        else {
            panic!()
        };
        assert_eq!(caption, &widen(b"Back"));
        assert_eq!((*extra_int, *extra_id), (1001, 393_251));
        assert_eq!(*style, Style::Default);
        assert_eq!(*bottom, 6.510_417e-2);
        assert_eq!(
            desc.entries[3].payload,
            EntryPayload::ScrollCycle {
                right: 2.27e-2,
                bottom: 0.36,
                extra: [0.0, 100.0, 14.0, 1.0],
                flag: false
            }
        );
    }

    #[test]
    fn a_bad_line_fails_the_load_and_keeps_what_was_read() {
        let desc = PageDesc::load(b"UIListBox A 0 0 1 1\nUIListBox B 0 0 1\nUIListBox C 0 0 1 1\n");
        assert!(!desc.ok);
        assert_eq!(desc.entries.len(), 1);
        assert_eq!(desc.aspect, 0.0);
        assert!(desc.data_entries().is_empty());
        assert!(!PageDesc::load(b"Unknown A 0 0 1 1\n").ok);
        // Comments, indented and empty lines are skipped.
        let desc = PageDesc::load(b"# x\n  UIListBox A 0 0 1 1\n\nUIListBox B 0 0 1 1");
        assert!(desc.ok);
        assert_eq!(desc.entries.len(), 1);
    }

    #[test]
    fn a_missing_message_float_reuses_the_previous_line() {
        let desc = PageDesc::load(
            b"UITextMsgButton A 0 0 1 1 \"a\" 7.9 \"<none>\"\nUITextMsgButton B 0 0 1 1 \"b\"\n",
        );
        assert!(desc.ok);
        let ints: Vec<(i32, i32)> = desc
            .entries
            .iter()
            .map(|e| match &e.payload {
                EntryPayload::TextMsgButton {
                    extra_int,
                    extra_id,
                    ..
                } => (*extra_int, *extra_id),
                _ => panic!(),
            })
            .collect();
        assert_eq!(ints, vec![(7, 0), (7, 0)]);
        assert!(matches!(
            &desc.entries[0].payload,
            EntryPayload::TextMsgButton {
                style: Style::None,
                ..
            }
        ));
        // A caption byte above 0x7F is sign-extended.
        assert_eq!(widen(&[0xE9]), vec![0xFFE9]);
    }
}
