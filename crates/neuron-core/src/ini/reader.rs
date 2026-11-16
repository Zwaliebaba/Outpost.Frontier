//! The game INI reader (work package P1.2).
//!
//! Ported from `INI_Reader`, `FileMap`, `TextFile32` and `LineParser32` in
//! `CppSource/Common/common_ini.cpp` and `common_textfile.cpp`. The reader
//! works on file contents the caller has read; opening files belongs to the
//! virtual file system (P1.3).
//!
//! Text values keep C string semantics: a NUL byte ends a name or a value.
//!
//! Every file is read as text. The C++ also decodes binary INI files; the
//! game data has none, so the port leaves that out (decision O13,
//! divergence D10).

use super::number::{is_number, parse_number, string_index};
use crate::hash::{c_str, eq_ignore_case};
use crate::msvcrt::atoi;

/// `TextFile32::read_line` reads at most this many bytes as one line.
const LINE_WINDOW: u32 = 0x3FF;

/// Something the C++ reports through `FDUMP`. The reader collects them;
/// [`IniReader::take_reports`] hands them on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IniReport {
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
}

/// A read position, saved by [`IniReader::state`] (`INI_Reader::State`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IniState {
    /// The byte offset.
    pub cursor: u32,
    /// The line counter.
    pub line_num: u32,
    /// The section name.
    pub header: Vec<u8>,
    /// The value name.
    pub name: Vec<u8>,
    /// The value text.
    pub params: Vec<u8>,
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

/// `INI_Reader`.
#[derive(Clone, Debug, Default)]
pub struct IniReader {
    text: TextFile,
    header: Vec<u8>,
    value_name: Vec<u8>,
    value_params: Vec<u8>,
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
    /// An empty file cannot be mapped and is refused, as in the C++.
    /// Returns whether the file is open.
    pub fn open(&mut self, name: &[u8], contents: Vec<u8>) -> bool {
        if contents.is_empty() {
            return false;
        }
        self.open_text(FileMap::new(name, contents));
        true
    }

    /// `open_memory`. An empty buffer is refused and leaves the reader as
    /// it was.
    pub fn open_memory(&mut self, contents: &[u8]) -> bool {
        if contents.is_empty() {
            return false;
        }
        self.open_text(FileMap::new(b"", contents.to_vec()));
        true
    }

    fn open_text(&mut self, map: FileMap) {
        self.text.map = map;
        self.text.line_num = 0;
        self.header.clear();
        self.text.map.seek(0);
    }

    /// `close`: releases the view the reader uses. Names and counters stay.
    pub fn close(&mut self) {
        self.text.map.close();
    }

    /// `reset`: back to the first section.
    pub fn reset(&mut self) {
        self.text.line_num = 0;
        self.header.clear();
        self.text.map.seek(0);
    }

    /// `seek`.
    pub fn seek(&mut self, position: u32) {
        self.text.map.seek(position);
    }

    /// `tell`.
    #[must_use]
    pub fn tell(&self) -> u32 {
        self.text.map.tell()
    }

    /// `get_line_num`: the number of lines read.
    #[must_use]
    pub fn line_num(&self) -> u32 {
        self.text.line_num
    }

    /// `is_end`.
    #[must_use]
    pub fn is_end(&self) -> bool {
        if self.text.map.view.is_some() {
            self.text.map.cursor >= self.text.map.size
        } else {
            self.text.map.size == 0
        }
    }

    /// `read_header`: moves to the next section.
    pub fn read_header(&mut self) -> bool {
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

    /// `read_value`: moves to the next value of the section.
    pub fn read_value(&mut self) -> bool {
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
        eq_ignore_case(&self.header, name)
    }

    /// `is_value`: the value name equals `name` under `stricmp`.
    #[must_use]
    pub fn is_value(&self, name: &[u8]) -> bool {
        eq_ignore_case(&self.value_name, name)
    }

    /// `get_header_ptr`: the section name.
    #[must_use]
    pub fn header(&self) -> Vec<u8> {
        self.header.clone()
    }

    /// `get_name_ptr`: the value name.
    #[must_use]
    pub fn name(&self) -> Vec<u8> {
        self.value_name.clone()
    }

    /// `get_file_name`.
    #[must_use]
    pub fn file_name(&self) -> &[u8] {
        &self.text.map.name
    }

    /// `get_line_ptr`: `"name = parameters"`.
    #[must_use]
    pub fn line(&self) -> Vec<u8> {
        let mut line = self.value_name.clone();
        line.extend_from_slice(b" = ");
        line.extend_from_slice(&self.value_params);
        line
    }

    /// `get_num_parameters`: the commas plus one, or zero for an empty
    /// value.
    #[must_use]
    pub fn num_parameters(&self) -> u32 {
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
        string_index(&self.value_params, index).is_empty()
    }

    /// `get_value_bool`.
    pub fn value_bool(&mut self, index: u32) -> bool {
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

    /// `get_value_int`: through the wrapping `atoi`.
    #[must_use]
    pub fn value_int(&self, index: u32) -> i32 {
        atoi(string_index(&self.value_params, index))
    }

    /// `get_value_float`: through `ParseNumber`.
    pub fn value_float(&mut self, index: u32) -> f64 {
        let field = string_index(&self.value_params, index);
        let parsed = parse_number(field);
        if parsed.illegal_digit {
            self.reports.push(IniReport::IllegalBaseNDigit {
                value: field.to_vec(),
            });
        }
        parsed.value
    }

    /// `get_value_string(index)`: one field.
    #[must_use]
    pub fn value_string_at(&self, index: u32) -> Vec<u8> {
        string_index(&self.value_params, index).to_vec()
    }

    /// `get_value_string()`: the whole value text.
    #[must_use]
    pub fn value_string(&self) -> Vec<u8> {
        self.value_params.clone()
    }

    /// `get_value_wstring`: [`IniReader::value_string`] widened byte by
    /// byte, as `mbstowcs` does in the "C" locale.
    #[must_use]
    pub fn value_wide(&self) -> Vec<u16> {
        self.value_params.iter().map(|&b| u16::from(b)).collect()
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
        IniState {
            cursor: self.text.map.tell(),
            line_num: self.text.line_num,
            header: self.header.clone(),
            name: self.value_name.clone(),
            params: self.value_params.clone(),
        }
    }

    /// `set_state`.
    pub fn set_state(&mut self, state: &IniState) {
        self.text.map.seek(state.cursor);
        self.text.line_num = state.line_num;
        self.header.clone_from(&state.header);
        self.value_name.clone_from(&state.name);
        self.value_params.clone_from(&state.params);
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

    #[test]
    fn empty_input_is_refused() {
        let mut reader = IniReader::new();
        assert!(!reader.open(b"x", Vec::new()));
        assert!(!reader.open_memory(b""));
    }
}
