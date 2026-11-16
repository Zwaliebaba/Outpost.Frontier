//! The game INI reader for text and BINI files (work package P1.2).
//!
//! Ported from `INI_Reader`, `FileMap`, `TextFile32` and `LineParser32` in
//! `CppSource/Common/common_ini.cpp` and `common_textfile.cpp`. The reader
//! works on file contents the caller has read; opening files belongs to the
//! virtual file system (P1.3).
//!
//! Text values keep C string semantics: a NUL byte ends a name or a value.

use super::number::{is_number, parse_number, string_index};
use crate::hash::{c_str, eq_ignore_case};
use crate::msvcrt::{atof, atoi, format_g};
use crate::num::trunc_to_i32_via_i64;

/// `TextFile32::read_line` reads at most this many bytes as one line.
const LINE_WINDOW: u32 = 0x3FF;
const BINI_MAGIC: &[u8; 4] = b"BINI";
const BINI_HEADER_BYTES: u32 = 0xC;
const BINI_PARAM_BYTES: u32 = 5;
/// The BINI join buffer, terminator included (`JoinChars`).
const JOIN_CHARS: usize = 0x1000;
/// The BINI `get_line_ptr` buffer, terminator included.
const BINI_LINE_CHARS: usize = 0x2000;
/// `get_value_wstring` converts at most this many BINI bytes.
const BINI_WIDE_CHARS: usize = 0x100;

/// Something the C++ reports through `FDUMP` or a message box. The reader
/// collects them; [`IniReader::take_reports`] hands them on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IniReport {
    /// A BINI getter asked for a parameter the value does not have. The C++
    /// shows a message box.
    MissingParameter {
        /// The section name.
        header: Vec<u8>,
        /// The value name.
        name: Vec<u8>,
        /// The parameter asked for.
        index: u32,
    },
    /// A text value is not one of `true`, `1`, `false`, `0` (an error).
    InvalidBool {
        /// The field.
        value: Vec<u8>,
        /// The file name.
        file: Vec<u8>,
    },
    /// `ParseNumber` met a digit too large for its base (a warning).
    IllegalBaseNDigit {
        /// The field.
        value: Vec<u8>,
    },
    /// `get_line_num` was called on a BINI file (a warning).
    LineNumberOfBini,
}

/// A read position, saved by [`IniReader::state`] (`INI_Reader::State`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IniState {
    /// A text file.
    Text {
        /// The byte offset.
        cursor: u32,
        /// The line counter.
        line_num: u32,
        /// The section name.
        header: Vec<u8>,
        /// The value name.
        name: Vec<u8>,
        /// The value text.
        params: Vec<u8>,
    },
    /// A BINI file. Offsets are into the file.
    Bini {
        /// The byte offset.
        cursor: u32,
        /// The current block record.
        block: Option<u32>,
        /// Values read in the block.
        value_index: i16,
        /// The current value record.
        value: Option<u32>,
        /// The current value's parameters.
        params: Option<u32>,
    },
}

/// `FileMap`: a read-only view with a cursor.
#[derive(Clone, Debug, Default)]
struct FileMap {
    name: Vec<u8>,
    view: Option<Vec<u8>>,
    size: u32,
    cursor: u32,
}

impl FileMap {
    fn new(name: &[u8], contents: Vec<u8>) -> Self {
        Self {
            name: name.to_vec(),
            size: u32::try_from(contents.len()).unwrap_or(u32::MAX),
            view: Some(contents),
            cursor: 0,
        }
    }

    fn close(&mut self) {
        self.name.clear();
        self.view = None;
        self.cursor = 0;
    }

    fn seek(&mut self, position: u32) {
        if self.view.is_some() {
            self.cursor = position.min(self.size);
        }
    }

    fn tell(&self) -> u32 {
        self.cursor
    }

    /// `request_pointer`: up to `bytes` bytes at the cursor. Returns their
    /// offset, or `None` (a null pointer) when nothing is left.
    fn request(&mut self, bytes: u32) -> (Option<u32>, u32) {
        if self.view.is_some() && self.cursor < self.size {
            let got = bytes.min(self.size - self.cursor);
            let start = self.cursor;
            self.cursor = start + got;
            (Some(start), got)
        } else {
            (None, 0)
        }
    }

    /// Bytes of the view; past its end the bytes read as zero, as in the
    /// zero-filled tail of a mapped page (divergence D6).
    fn byte(&self, offset: u64) -> u8 {
        let view = self.view.as_deref().unwrap_or_default();
        usize::try_from(offset)
            .ok()
            .and_then(|o| view.get(o))
            .copied()
            .unwrap_or(0)
    }

    fn u16_at(&self, offset: u32) -> u16 {
        let offset = u64::from(offset);
        u16::from_le_bytes([self.byte(offset), self.byte(offset + 1)])
    }

    fn u32_at(&self, offset: u64) -> u32 {
        u32::from_le_bytes([
            self.byte(offset),
            self.byte(offset + 1),
            self.byte(offset + 2),
            self.byte(offset + 3),
        ])
    }

    /// The NUL-terminated string at `offset`; it ends at the end of the
    /// view at the latest (divergence D6).
    fn c_string(&self, offset: u64) -> Vec<u8> {
        let view = self.view.as_deref().unwrap_or_default();
        match usize::try_from(offset).ok().and_then(|o| view.get(o..)) {
            Some(rest) => c_str(rest).to_vec(),
            None => Vec::new(),
        }
    }
}

/// `LineParser32`: one line and a read position. The byte at `len` reads
/// as NUL.
struct Line {
    bytes: Vec<u8>,
    position: usize,
}

impl Line {
    fn at(&self, index: usize) -> u8 {
        self.bytes.get(index).copied().unwrap_or(0)
    }

    fn skip_white(&mut self) {
        while matches!(self.at(self.position), b' ' | b'\t') {
            self.position += 1;
        }
    }

    /// `read_terminator2` (and `read_terminator` with one terminator): the
    /// run before a terminator, the position left on it, trailing blanks
    /// dropped, then cut at a NUL byte as the C string is.
    fn read_until(&mut self, first: u8, second: u8) -> Vec<u8> {
        let rest = self.bytes.get(self.position..).unwrap_or_default();
        let count = rest
            .iter()
            .position(|&b| b == first || b == second)
            .unwrap_or(rest.len());
        let mut copy = &rest[..count];
        self.position += count;
        while let [head @ .., b' ' | b'\t'] = copy {
            copy = head;
        }
        c_str(copy).to_vec()
    }
}

/// `TextFile32`: a file map read line by line.
#[derive(Clone, Debug, Default)]
struct TextFile {
    map: FileMap,
    line_num: u32,
}

impl TextFile {
    fn read_line(&mut self) -> Option<Line> {
        let start = if self.map.view.is_some() {
            self.map.cursor
        } else {
            0
        };
        self.line_num = self.line_num.wrapping_add(1);
        let (offset, window) = self.map.request(LINE_WINDOW);
        let offset = offset? as usize;
        let window_bytes =
            &self.map.view.as_deref().unwrap_or_default()[offset..offset + window as usize];
        let (text_length, consumed) =
            match window_bytes.iter().position(|&b| b == b'\r' || b == b'\n') {
                Some(index) => {
                    let partner = window_bytes[index] ^ 7;
                    let next = if window_bytes.get(index + 1) == Some(&partner) {
                        index + 2
                    } else {
                        index + 1
                    };
                    (index, next)
                }
                None => (window_bytes.len(), window_bytes.len()),
            };
        let line = Line {
            bytes: window_bytes[..text_length].to_vec(),
            position: 0,
        };
        let new_cursor = start.saturating_add(u32::try_from(consumed).unwrap_or(u32::MAX));
        self.map.cursor = new_cursor.min(self.map.size);
        Some(line)
    }
}

/// The BINI half of the reader.
#[derive(Clone, Debug, Default)]
struct BiniFile {
    map: FileMap,
    string_table: u32,
    block: Option<u32>,
    value_index: i16,
    value: Option<u32>,
    params: Option<u32>,
    /// The last number `FormatParam` wrote. The C++ buffer is shared by the
    /// whole process (divergence D7).
    number_text: Vec<u8>,
}

impl BiniFile {
    fn string(&self, offset: u32) -> Vec<u8> {
        self.map
            .c_string(u64::from(self.string_table) + u64::from(offset))
    }

    fn block_name(&self) -> Vec<u8> {
        self.block
            .map(|b| self.string(self.map.u16_at(b).into()))
            .unwrap_or_default()
    }

    fn block_value_count(&self) -> u16 {
        self.block.map_or(0, |b| self.map.u16_at(b + 2))
    }

    fn value_name(&self) -> Vec<u8> {
        self.value
            .map(|v| self.string(self.map.u16_at(v).into()))
            .unwrap_or_default()
    }

    fn param_count(&self) -> u8 {
        self.value.map_or(0, |v| self.map.byte(u64::from(v) + 2))
    }

    fn param(&self, index: u32) -> Param {
        let base =
            u64::from(self.params.unwrap_or(0)) + u64::from(index) * u64::from(BINI_PARAM_BYTES);
        let payload = self.map.u32_at(base + 1);
        match self.map.byte(base) {
            0 => Param::Byte(payload.to_le_bytes()[0]),
            1 => Param::Int(i32::from_le_bytes(payload.to_le_bytes())),
            2 => Param::Float(f32::from_bits(payload)),
            3 => Param::String(payload),
            _ => Param::Unknown,
        }
    }

    fn format_param(&mut self, index: u32, reports: &mut Vec<IniReport>) -> Vec<u8> {
        if self.value.is_none() {
            return Vec::new();
        }
        if index >= u32::from(self.param_count()) {
            reports.push(self.missing(index));
            return Vec::new();
        }
        let text = match self.param(index) {
            Param::Byte(b) => b.to_string().into_bytes(),
            Param::Int(i) => i.to_string().into_bytes(),
            Param::Float(f) => format_g(f64::from(f), 6).into_bytes(),
            Param::String(offset) => return self.string(offset),
            Param::Unknown => return self.number_text.clone(),
        };
        self.number_text.clone_from(&text);
        text
    }

    fn join(&mut self, reports: &mut Vec<IniReport>) -> Vec<u8> {
        let mut joined = Vec::new();
        for index in 0..u32::from(self.param_count()) {
            let piece = self.format_param(index, reports);
            if index > 0 {
                append_bounded(&mut joined, JOIN_CHARS, b", ");
            }
            append_bounded(&mut joined, JOIN_CHARS, &piece);
        }
        joined
    }

    fn missing(&self, index: u32) -> IniReport {
        IniReport::MissingParameter {
            header: self.block_name(),
            name: self.value_name(),
            index,
        }
    }
}

#[derive(Clone, Copy)]
enum Param {
    Byte(u8),
    Int(i32),
    Float(f32),
    String(u32),
    Unknown,
}

fn append_bounded(dest: &mut Vec<u8>, chars: usize, src: &[u8]) {
    let room = (chars - 1).saturating_sub(dest.len());
    dest.extend_from_slice(&src[..src.len().min(room)]);
}

/// `INI_Reader`.
#[derive(Clone, Debug, Default)]
pub struct IniReader {
    text: TextFile,
    header: Vec<u8>,
    value_name: Vec<u8>,
    value_params: Vec<u8>,
    bini: BiniFile,
    is_bini: bool,
    reports: Vec<IniReport>,
}

impl IniReader {
    /// A reader with nothing open.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `open` after the file was read. The caller reads the file, reports a
    /// fatal error when it must exist, and refreshes the loading screen.
    ///
    /// A BINI file is one that starts with a
    /// full BINI header whose string table lies inside the file, text
    /// otherwise. An empty file cannot be mapped and is refused, as in the
    /// C++. Returns whether the file is open.
    pub fn open(&mut self, name: &[u8], contents: Vec<u8>) -> bool {
        if contents.is_empty() {
            return false;
        }
        let is_bini = contents.len() >= BINI_HEADER_BYTES as usize
            && &contents[..4] == BINI_MAGIC
            && u32::from_le_bytes([contents[8], contents[9], contents[10], contents[11]])
                < u32::try_from(contents.len()).unwrap_or(u32::MAX);
        let map = FileMap::new(name, contents);
        if is_bini {
            self.bini.string_table = map.u32_at(8);
            self.bini.map = map;
            self.bini.map.cursor = BINI_HEADER_BYTES;
            self.bini.block = None;
            self.bini.value = None;
            self.is_bini = true;
        } else {
            self.open_text(map);
        }
        true
    }

    /// `open_memory`: always text. An empty buffer is refused and leaves
    /// the reader as it was.
    pub fn open_memory(&mut self, contents: &[u8]) -> bool {
        if contents.is_empty() {
            return false;
        }
        self.open_text(FileMap::new(b"", contents.to_vec()));
        true
    }

    fn open_text(&mut self, map: FileMap) {
        self.is_bini = false;
        self.text.map = map;
        self.text.line_num = 0;
        self.header.clear();
        self.text.map.seek(0);
    }

    /// `close`: releases the view the reader uses. Names and counters stay.
    pub fn close(&mut self) {
        if self.is_bini {
            self.bini.map.close();
        } else {
            self.text.map.close();
        }
    }

    /// `reset`: back to the first section.
    pub fn reset(&mut self) {
        if self.is_bini {
            self.bini.block = None;
            self.bini.value = None;
            self.bini.map.seek(BINI_HEADER_BYTES);
        } else {
            self.text.line_num = 0;
            self.header.clear();
            self.text.map.seek(0);
        }
    }

    /// `seek`.
    pub fn seek(&mut self, position: u32) {
        if self.is_bini {
            self.bini.map.seek(position);
        } else {
            self.text.map.seek(position);
        }
    }

    /// `tell`.
    #[must_use]
    pub fn tell(&self) -> u32 {
        if self.is_bini {
            self.bini.map.tell()
        } else {
            self.text.map.tell()
        }
    }

    /// `get_line_num`: the number of lines read. A BINI file has none and
    /// reports a warning.
    pub fn line_num(&mut self) -> u32 {
        if self.is_bini {
            self.reports.push(IniReport::LineNumberOfBini);
            return 0;
        }
        self.text.line_num
    }

    /// Whether the open file is BINI.
    #[must_use]
    pub fn is_bini(&self) -> bool {
        self.is_bini
    }

    /// `is_end`.
    #[must_use]
    pub fn is_end(&self) -> bool {
        if self.is_bini {
            let table = self.bini.string_table;
            return if self.bini.map.view.is_some() {
                self.bini.map.cursor >= table
            } else {
                table == 0
            };
        }
        if self.text.map.view.is_some() {
            self.text.map.cursor >= self.text.map.size
        } else {
            self.text.map.size == 0
        }
    }

    /// `read_header`: moves to the next section.
    pub fn read_header(&mut self) -> bool {
        if self.is_bini {
            self.read_bini_header()
        } else {
            self.read_text_header()
        }
    }

    fn read_text_header(&mut self) -> bool {
        while let Some(mut line) = self.text.read_line() {
            line.skip_white();
            if line.position < line.bytes.len() && line.bytes[line.position] == b'[' {
                line.position += 1;
                self.header = line.read_until(b']', b']');
                if self.header.first() != Some(&b';') {
                    return true;
                }
            }
        }
        self.header.clear();
        false
    }

    fn read_bini_header(&mut self) -> bool {
        let bini = &mut self.bini;
        if bini.block.is_some() {
            let count = i32::from(bini.block_value_count());
            while i32::from(bini.value_index) < count {
                let (offset, got) = bini.map.request(3);
                if got == 3 {
                    bini.value = offset;
                    let skip = u32::from(bini.param_count()) * BINI_PARAM_BYTES;
                    let position = bini.map.tell().saturating_add(skip);
                    bini.map.seek(position);
                } else {
                    bini.value = None;
                }
                bini.value_index = bini.value_index.wrapping_add(1);
                if bini.value_index == i16::MIN {
                    // The C++ loops for ever here (divergence D8).
                    break;
                }
            }
        }
        bini.block = None;
        bini.value = None;
        if bini.map.tell() >= bini.string_table {
            return false;
        }
        let (offset, got) = bini.map.request(4);
        if got != 4 {
            bini.block = None;
            return false;
        }
        bini.block = offset;
        bini.value_index = 0;
        true
    }

    /// `read_value`: moves to the next value of the section. Without a
    /// current BINI block it returns false where the C++ reads through a
    /// null pointer; the other BINI getters treat a missing value the same
    /// way (divergence D5).
    pub fn read_value(&mut self) -> bool {
        if !self.is_bini {
            return self.read_text_value();
        }
        let bini = &mut self.bini;
        if bini.block.is_none()
            || i32::from(bini.value_index) >= i32::from(bini.block_value_count())
        {
            return false;
        }
        let (offset, got) = bini.map.request(3);
        if got != 3 {
            bini.value = None;
            return false;
        }
        bini.value = offset;
        let bytes = u32::from(bini.param_count()) * BINI_PARAM_BYTES;
        bini.params = bini.map.request(bytes).0;
        bini.value_index = bini.value_index.wrapping_add(1);
        true
    }

    fn read_text_value(&mut self) -> bool {
        loop {
            let saved_line = self.text.line_num;
            let saved_cursor = self.text.map.tell();
            let Some(mut line) = self.text.read_line() else {
                self.value_name.clear();
                return false;
            };
            line.skip_white();
            if line.position >= line.bytes.len() {
                continue;
            }
            match line.bytes[line.position] {
                0 | b';' => continue,
                b'[' => {
                    self.text.line_num = saved_line;
                    self.text.map.seek(saved_cursor);
                    return false;
                }
                _ => {}
            }
            self.value_name = line.read_until(b'=', b';');
            if line.at(line.position) != b'=' || line.position >= line.bytes.len() {
                self.value_params.clear();
                return true;
            }
            line.position += 1;
            line.skip_white();
            self.value_params = line.read_until(b';', b';');
            return true;
        }
    }

    /// `find_header`: from the start, the first section named `name`.
    pub fn find_header(&mut self, name: &[u8]) -> bool {
        self.reset();
        while self.read_header() {
            if self.is_header(name) {
                return true;
            }
        }
        false
    }

    /// `is_header`: the section name equals `name` under `stricmp`.
    #[must_use]
    pub fn is_header(&self, name: &[u8]) -> bool {
        eq_ignore_case(&self.header(), name)
    }

    /// `is_value`: the value name equals `name` under `stricmp`.
    #[must_use]
    pub fn is_value(&self, name: &[u8]) -> bool {
        eq_ignore_case(&self.name(), name)
    }

    /// `get_header_ptr`: the section name.
    #[must_use]
    pub fn header(&self) -> Vec<u8> {
        if self.is_bini {
            self.bini.block_name()
        } else {
            self.header.clone()
        }
    }

    /// `get_name_ptr`: the value name.
    #[must_use]
    pub fn name(&self) -> Vec<u8> {
        if self.is_bini {
            self.bini.value_name()
        } else {
            self.value_name.clone()
        }
    }

    /// `get_file_name`.
    #[must_use]
    pub fn file_name(&self) -> &[u8] {
        if self.is_bini {
            &self.bini.map.name
        } else {
            &self.text.map.name
        }
    }

    /// `get_line_ptr`: `"name = parameters"`.
    pub fn line(&mut self) -> Vec<u8> {
        let mut line = self.name();
        if self.is_bini {
            if self.bini.value.is_none() {
                return Vec::new();
            }
            line.extend_from_slice(b" = ");
            line.truncate(BINI_LINE_CHARS - 1);
            let joined = self.bini.join(&mut self.reports);
            append_bounded(&mut line, BINI_LINE_CHARS, &joined);
        } else {
            line.extend_from_slice(b" = ");
            line.extend_from_slice(&self.value_params);
        }
        line
    }

    /// `get_num_parameters`: for text, the commas plus one, or zero for an
    /// empty value.
    #[must_use]
    pub fn num_parameters(&self) -> u32 {
        if self.is_bini {
            return u32::from(self.bini.param_count());
        }
        if self.value_params.is_empty() {
            0
        } else {
            let commas = self.value_params.iter().filter(|&&b| b == b',').count();
            u32::try_from(commas + 1).unwrap_or(u32::MAX)
        }
    }

    /// `is_value_empty`.
    #[must_use]
    pub fn is_value_empty(&self, index: u32) -> bool {
        if self.is_bini {
            return self.bini.value.is_none() || index >= u32::from(self.bini.param_count());
        }
        string_index(&self.value_params, index).is_empty()
    }

    /// The BINI parameter `index`, reporting a missing one.
    fn bini_param(&mut self, index: u32) -> Option<Param> {
        self.bini.value?;
        if index >= u32::from(self.bini.param_count()) {
            let report = self.bini.missing(index);
            self.reports.push(report);
            return None;
        }
        Some(self.bini.param(index))
    }

    /// `get_value_bool`.
    pub fn value_bool(&mut self, index: u32) -> bool {
        if self.is_bini {
            return match self.bini_param(index) {
                Some(Param::Byte(b)) => b != 0,
                Some(Param::Int(i)) => i != 0,
                Some(Param::Float(f)) => f != 0.0,
                Some(Param::String(offset)) => {
                    let field = self.bini.string(offset);
                    if eq_ignore_case(&field, b"true") {
                        true
                    } else if eq_ignore_case(&field, b"false") {
                        false
                    } else {
                        atoi(&field) != 0
                    }
                }
                Some(Param::Unknown) | None => false,
            };
        }
        let field = string_index(&self.value_params, index);
        if eq_ignore_case(field, b"true") || eq_ignore_case(field, b"1") {
            return true;
        }
        if eq_ignore_case(field, b"false") || eq_ignore_case(field, b"0") {
            return false;
        }
        self.reports.push(IniReport::InvalidBool {
            value: field.to_vec(),
            file: self.text.map.name.clone(),
        });
        false
    }

    /// `get_value_int`: text through the wrapping `atoi`.
    pub fn value_int(&mut self, index: u32) -> i32 {
        if self.is_bini {
            return match self.bini_param(index) {
                Some(Param::Byte(b)) => i32::from(b),
                Some(Param::Int(i)) => i,
                Some(Param::Float(f)) => trunc_to_i32_via_i64(f64::from(f)),
                Some(Param::String(offset)) => atoi(&self.bini.string(offset)),
                Some(Param::Unknown) | None => 0,
            };
        }
        atoi(string_index(&self.value_params, index))
    }

    /// `get_value_float`: text through `ParseNumber`.
    pub fn value_float(&mut self, index: u32) -> f64 {
        if self.is_bini {
            return match self.bini_param(index) {
                Some(Param::Byte(b)) => f64::from(b),
                Some(Param::Int(i)) => f64::from(i),
                Some(Param::Float(f)) => f64::from(f),
                Some(Param::String(offset)) => atof(&self.bini.string(offset)),
                Some(Param::Unknown) | None => 0.0,
            };
        }
        let field = string_index(&self.value_params, index);
        let parsed = parse_number(field);
        if parsed.illegal_digit {
            self.reports.push(IniReport::IllegalBaseNDigit {
                value: field.to_vec(),
            });
        }
        parsed.value
    }

    /// `get_value_string(index)`: one field. BINI numbers are formatted
    /// with `%d` and `%g`.
    pub fn value_string_at(&mut self, index: u32) -> Vec<u8> {
        if self.is_bini {
            return self.bini.format_param(index, &mut self.reports);
        }
        string_index(&self.value_params, index).to_vec()
    }

    /// `get_value_string()`: the whole value text; BINI parameters joined
    /// with `", "`.
    pub fn value_string(&mut self) -> Vec<u8> {
        if self.is_bini {
            return self.bini.join(&mut self.reports);
        }
        self.value_params.clone()
    }

    /// `get_value_wstring`: [`IniReader::value_string`] widened byte by
    /// byte, as `mbstowcs` does in the "C" locale.
    pub fn value_wide(&mut self) -> Vec<u16> {
        let mut bytes = self.value_string();
        if self.is_bini {
            bytes.truncate(BINI_WIDE_CHARS);
        }
        bytes.iter().map(|&b| u16::from(b)).collect()
    }

    /// `get_vector`: the first three parameters as floats.
    pub fn vector(&mut self) -> [f32; 3] {
        [0, 1, 2].map(|index| self.value_float(index) as f32)
    }

    /// `is_number`.
    #[must_use]
    pub fn is_number(&self, candidate: &[u8]) -> bool {
        is_number(candidate)
    }

    /// `get_state`.
    #[must_use]
    pub fn state(&self) -> IniState {
        if self.is_bini {
            IniState::Bini {
                cursor: self.bini.map.tell(),
                block: self.bini.block,
                value_index: self.bini.value_index,
                value: self.bini.value,
                params: self.bini.params,
            }
        } else {
            IniState::Text {
                cursor: self.text.map.tell(),
                line_num: self.text.line_num,
                header: self.header.clone(),
                name: self.value_name.clone(),
                params: self.value_params.clone(),
            }
        }
    }

    /// `set_state`. A state of the other file kind is ignored (divergence
    /// D9).
    pub fn set_state(&mut self, state: &IniState) {
        match (self.is_bini, state) {
            (
                true,
                IniState::Bini {
                    cursor,
                    block,
                    value_index,
                    value,
                    params,
                },
            ) => {
                self.bini.map.seek(*cursor);
                self.bini.block = *block;
                self.bini.value_index = *value_index;
                self.bini.value = *value;
                self.bini.params = *params;
            }
            (
                false,
                IniState::Text {
                    cursor,
                    line_num,
                    header,
                    name,
                    params,
                },
            ) => {
                self.text.map.seek(*cursor);
                self.text.line_num = *line_num;
                self.header.clone_from(header);
                self.value_name.clone_from(name);
                self.value_params.clone_from(params);
            }
            _ => {}
        }
    }

    /// The reports collected since the last call.
    pub fn take_reports(&mut self) -> Vec<IniReport> {
        core::mem::take(&mut self.reports)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text_reader(source: &[u8]) -> IniReader {
        let mut reader = IniReader::new();
        assert!(reader.open(b"test.ini", source.to_vec()));
        reader
    }

    #[test]
    fn text_sections_and_values() {
        let mut reader = text_reader(b"; c\n  [Ship] ; x\nnickname = li_elite ; tail\n\n  flag\n[;old]\n[Next]\nhp = 1, 2.5 ,0x10\n");
        assert!(reader.read_header());
        assert_eq!(reader.header(), b"Ship");
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"nickname");
        assert_eq!(reader.value_string(), b"li_elite");
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"flag");
        assert_eq!(reader.num_parameters(), 0);
        assert!(!reader.read_value());
        assert!(reader.read_header());
        assert_eq!(reader.header(), b"Next");
        assert!(reader.read_value());
        assert_eq!(reader.num_parameters(), 3);
        assert_eq!(reader.value_int(0), 1);
        assert_eq!(reader.value_float(1), 2.5);
        assert_eq!(reader.value_float(2), 16.0);
        assert_eq!(reader.value_string_at(1), b"2.5");
        assert_eq!(reader.line(), b"hp = 1, 2.5 ,0x10");
        assert!(!reader.read_header());
        assert!(reader.is_end());
    }

    #[test]
    fn line_endings_and_line_numbers() {
        let mut reader = text_reader(b"[a]\r\nx=1\n\ry=2\r\r\n");
        assert!(reader.read_header());
        assert!(reader.read_value());
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"y");
        assert_eq!(reader.line_num(), 3);
        assert!(!reader.read_value());
    }

    #[test]
    fn find_header_and_state() {
        let mut reader = text_reader(b"[a]\nx=1\n[B]\ny=2\nz=3\n");
        assert!(reader.find_header(b"b"));
        assert!(reader.read_value());
        let saved = reader.state();
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"z");
        reader.set_state(&saved);
        assert_eq!(reader.name(), b"y");
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"z");
    }

    #[test]
    fn invalid_bool_is_reported() {
        let mut reader = text_reader(b"[a]\nx=yes,TRUE\n");
        reader.read_header();
        reader.read_value();
        assert!(!reader.value_bool(0));
        assert!(reader.value_bool(1));
        assert_eq!(
            reader.take_reports(),
            [IniReport::InvalidBool {
                value: b"yes".to_vec(),
                file: b"test.ini".to_vec()
            }]
        );
    }

    fn bini_file() -> Vec<u8> {
        // header, block "sec" with two values, string table.
        let strings = b"sec\0v\0w\0hello\0";
        let mut body = Vec::new();
        body.extend_from_slice(&0u16.to_le_bytes());
        body.extend_from_slice(&2u16.to_le_bytes());
        body.extend_from_slice(&4u16.to_le_bytes());
        body.push(4);
        body.push(0);
        body.extend_from_slice(&[200, 0, 0, 0]);
        body.push(1);
        body.extend_from_slice(&(-7i32).to_le_bytes());
        body.push(2);
        body.extend_from_slice(&1.5f32.to_bits().to_le_bytes());
        body.push(3);
        body.extend_from_slice(&8u32.to_le_bytes());
        body.extend_from_slice(&6u16.to_le_bytes());
        body.push(0);
        let mut file = b"BINI".to_vec();
        file.extend_from_slice(&1u32.to_le_bytes());
        file.extend_from_slice(&(12 + u32::try_from(body.len()).unwrap_or(0)).to_le_bytes());
        file.extend_from_slice(&body);
        file.extend_from_slice(strings);
        file
    }

    #[test]
    fn bini_values() {
        let mut reader = IniReader::new();
        assert!(reader.open(b"test.bini", bini_file()));
        assert!(reader.is_bini());
        assert!(reader.read_header());
        assert_eq!(reader.header(), b"sec");
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"v");
        assert_eq!(reader.num_parameters(), 4);
        assert_eq!(reader.value_string(), b"200, -7, 1.5, hello");
        assert_eq!(reader.value_int(2), 1);
        assert_eq!(reader.value_float(0), 200.0);
        assert!(reader.value_bool(1));
        assert_eq!(reader.line(), b"v = 200, -7, 1.5, hello");
        assert_eq!(reader.value_string_at(9), b"");
        assert_eq!(
            reader.take_reports(),
            [IniReport::MissingParameter {
                header: b"sec".to_vec(),
                name: b"v".to_vec(),
                index: 9
            }]
        );
        assert!(reader.read_value());
        assert_eq!(reader.name(), b"w");
        assert!(!reader.read_value());
        assert!(!reader.read_header());
        assert!(reader.is_end());
        assert!(reader.find_header(b"SEC"));
    }

    #[test]
    fn empty_input_is_refused() {
        let mut reader = IniReader::new();
        assert!(!reader.open(b"x", Vec::new()));
        assert!(!reader.open_memory(b""));
    }
}
