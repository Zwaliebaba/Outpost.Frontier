//! The engine INI parser (work package P1.2).
//!
//! Ported from `FIniLexer`, `FIniDatabase` and `FIniSection` in
//! `CppSource/neuroncomponents/neuroncomponents.cpp`: the built-in profile
//! parser that reads the engine configuration. Sections are kept sorted by
//! name under [`stricmp`]; a section that appears twice is kept twice, the
//! later one behind the earlier. Values are byte strings.

use core::cmp::Ordering;

use crate::hash::{c_str, eq_ignore_case, stricmp};

/// The source name the C++ gives a parse from memory.
pub const LOCAL_BUFFER_NAME: &[u8] = b"(local buffer)";

/// Phrases (section names, key tails, values) hold at most this much.
const PHRASE_LIMIT: usize = 0x400;

/// `@include` files nest at most this deep. The C++ has no limit and
/// overflows its stack on an include cycle (divergence D3).
const MAX_INCLUDE_DEPTH: usize = 64;

/// Loads the file an `@include` names. The path is the byte string the
/// C++ passes to `fopen`. `None` means the file could not be opened, which
/// the parser ignores as the C++ does.
pub trait IncludeLoader {
    /// The contents of the file at `path`.
    fn load(&mut self, path: &[u8]) -> Option<Vec<u8>>;
}

impl<F: FnMut(&[u8]) -> Option<Vec<u8>>> IncludeLoader for F {
    fn load(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        self(path)
    }
}

/// An [`IncludeLoader`] for which no file exists.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoIncludes;

impl IncludeLoader for NoIncludes {
    fn load(&mut self, _path: &[u8]) -> Option<Vec<u8>> {
        None
    }
}

/// Why a parse stopped. Everything parsed before stays in the database, as
/// in the C++.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EngineIniError {
    /// `@include` was not followed by a file name. The C++ returns failure.
    IncludeWithoutName,
    /// Input on which the C++ executes a breakpoint (`__debugbreak`): a
    /// phrase too long for its buffer, a key whose value runs past it, or a
    /// stray `]` or `=` where a line should start (divergence D1).
    Breakpoint,
    /// A key before the first section. The C++ dereferences a null section
    /// (divergence D1).
    KeyBeforeSection,
    /// `@include` files nested deeper than the limit (divergence D3).
    IncludeTooDeep,
}

impl core::fmt::Display for EngineIniError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(match self {
            Self::IncludeWithoutName => "@include without a file name",
            Self::Breakpoint => "input the engine parser stops on",
            Self::KeyBeforeSection => "key before the first section",
            Self::IncludeTooDeep => "@include files nested too deep",
        })
    }
}

impl std::error::Error for EngineIniError {}

/// One `key = value` line, or a bare key.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EngineEntry {
    /// The key: the words before `=`, joined by single spaces.
    pub key: Vec<u8>,
    /// The value, or `None` for a line without `=`. An `=` with nothing
    /// after it gives an empty value.
    pub value: Option<Vec<u8>>,
}

/// One section.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EngineSection {
    /// The name between the brackets, words joined by single spaces.
    pub name: Vec<u8>,
    /// The lines in file order.
    pub entries: Vec<EngineEntry>,
}

impl EngineSection {
    /// `ReadLine`: line `index` (from 0) as `"key = value"`, or the bare
    /// key.
    #[must_use]
    pub fn line(&self, index: usize) -> Option<Vec<u8>> {
        let entry = self.entries.get(index)?;
        let mut line = entry.key.clone();
        if let Some(value) = &entry.value {
            line.extend_from_slice(b" = ");
            line.extend_from_slice(value);
        }
        Some(line)
    }

    /// `GetString`: the value of the first entry whose key equals `key`
    /// under [`stricmp`]. A bare key gives an empty value.
    #[must_use]
    pub fn value(&self, key: &[u8]) -> Option<&[u8]> {
        self.entries
            .iter()
            .find(|entry| eq_ignore_case(&entry.key, key))
            .map(|entry| entry.value.as_deref().unwrap_or_default())
    }
}

/// The parsed engine configuration.
#[derive(Clone, Debug, Default)]
pub struct EngineIni {
    sections: Vec<EngineSection>,
    current: Option<usize>,
}

impl EngineIni {
    /// An empty database.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `LoadFromMemory`: empties the database and parses `source`, which
    /// is named `"(local buffer)"` for relative includes.
    ///
    /// # Errors
    ///
    /// The parse stopped early; see [`EngineIniError`].
    pub fn load_buffer(
        &mut self,
        source: &[u8],
        loader: &mut dyn IncludeLoader,
    ) -> Result<(), EngineIniError> {
        self.load_named(LOCAL_BUFFER_NAME, source, loader)
    }

    /// `LoadFile` after the file was read: empties the database and parses
    /// `source`, named `path` for relative includes.
    ///
    /// # Errors
    ///
    /// The parse stopped early; see [`EngineIniError`].
    pub fn load_named(
        &mut self,
        path: &[u8],
        source: &[u8],
        loader: &mut dyn IncludeLoader,
    ) -> Result<(), EngineIniError> {
        self.sections.clear();
        self.current = None;
        self.parse(source, path, loader, 0)
    }

    /// All sections in their sorted order (`EnumerateSections`).
    #[must_use]
    pub fn sections(&self) -> &[EngineSection] {
        &self.sections
    }

    /// `OpenSection`: the first section whose name equals `name` under
    /// [`stricmp`].
    #[must_use]
    pub fn section(&self, name: &[u8]) -> Option<&EngineSection> {
        for section in &self.sections {
            match stricmp(&section.name, name) {
                Ordering::Equal => return Some(section),
                Ordering::Greater => return None,
                Ordering::Less => {}
            }
        }
        None
    }

    fn add_section(&mut self, name: Vec<u8>) {
        let position = self
            .sections
            .iter()
            .position(|s| stricmp(&s.name, &name) == Ordering::Greater)
            .unwrap_or(self.sections.len());
        self.sections.insert(
            position,
            EngineSection {
                name,
                entries: Vec::new(),
            },
        );
        self.current = Some(position);
    }

    fn add_entry(&mut self, key: Vec<u8>, value: Option<Vec<u8>>) -> Result<(), EngineIniError> {
        let index = self.current.ok_or(EngineIniError::KeyBeforeSection)?;
        self.sections[index]
            .entries
            .push(EngineEntry { key, value });
        Ok(())
    }

    fn parse(
        &mut self,
        source: &[u8],
        source_name: &[u8],
        loader: &mut dyn IncludeLoader,
        depth: usize,
    ) -> Result<(), EngineIniError> {
        let mut lexer = Lexer::new(source);
        let mut token = lexer.next_token();
        while token != Token::End {
            match token {
                Token::Word => {
                    let first = c_str(&lexer.text).to_vec();
                    let (tail_end, tail) = read_phrase(&mut lexer, Token::Equals);
                    match tail_end {
                        PhraseEnd::Stop => {
                            let (value_end, value) = read_phrase(&mut lexer, Token::Newline);
                            if value_end != PhraseEnd::Newline {
                                return Err(EngineIniError::Breakpoint);
                            }
                            self.add_entry(build_key(first, &tail), Some(value))?;
                        }
                        PhraseEnd::Newline => self.add_entry(build_key(first, &tail), None)?,
                        PhraseEnd::Overflow => return Err(EngineIniError::Breakpoint),
                    }
                }
                Token::Include => {
                    if lexer.next_token() != Token::Word {
                        return Err(EngineIniError::IncludeWithoutName);
                    }
                    let name = c_str(&lexer.text).to_vec();
                    let path = include_path(&name, source_name);
                    if depth + 1 > MAX_INCLUDE_DEPTH {
                        return Err(EngineIniError::IncludeTooDeep);
                    }
                    if let Some(contents) = loader.load(&path) {
                        match self.parse(&contents, &path, loader, depth + 1) {
                            // The C++ ignores the result of an included parse;
                            // only a crash in it ends the outer one.
                            Ok(()) | Err(EngineIniError::IncludeWithoutName) => {}
                            Err(error) => return Err(error),
                        }
                    }
                    lexer.advance();
                }
                Token::OpenBracket => {
                    let (end, name) = read_phrase(&mut lexer, Token::CloseBracket);
                    match end {
                        PhraseEnd::Stop => self.add_section(name),
                        PhraseEnd::Newline => {}
                        PhraseEnd::Overflow => return Err(EngineIniError::Breakpoint),
                    }
                }
                Token::Newline => {}
                Token::CloseBracket | Token::Equals | Token::End => {
                    return Err(EngineIniError::Breakpoint);
                }
            }
            token = lexer.next_token();
        }
        Ok(())
    }
}

fn build_key(mut first: Vec<u8>, tail: &[u8]) -> Vec<u8> {
    if !tail.is_empty() {
        first.push(b' ');
        first.extend_from_slice(tail);
    }
    first
}

/// The drive and directory parts `_splitpath` returns. The C++ ends the
/// process on a directory of 256 bytes or more (divergence D4).
fn drive_and_directory(path: &[u8]) -> (&[u8], &[u8]) {
    let (drive, rest) = if path.len() >= 2 && path[1] == b':' {
        path.split_at(2)
    } else {
        (&path[..0], path)
    };
    let directory = match rest.iter().rposition(|&b| b == b'/' || b == b'\\') {
        Some(last) => &rest[..=last],
        None => &rest[..0],
    };
    (drive, directory)
}

/// The path of an include: relative unless it has a drive or starts with
/// a backslash, and then next to the file being parsed.
fn include_path(name: &[u8], source_name: &[u8]) -> Vec<u8> {
    let (drive, directory) = drive_and_directory(name);
    if drive.is_empty() && directory.first() != Some(&b'\\') {
        let (drive, directory) = drive_and_directory(source_name);
        let mut path = drive.to_vec();
        path.extend_from_slice(directory);
        path.extend_from_slice(name);
        path
    } else {
        name.to_vec()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Token {
    Word,
    Include,
    OpenBracket,
    CloseBracket,
    Equals,
    Newline,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PhraseEnd {
    Stop,
    Newline,
    Overflow,
}

/// `ReadPhrase`: tokens up to `stop`, a newline or the end, joined by
/// single spaces. The length check counts whole words, the text keeps C
/// string semantics, so a word with a NUL byte cuts the phrase there.
fn read_phrase(lexer: &mut Lexer<'_>, stop: Token) -> (PhraseEnd, Vec<u8>) {
    let mut out = Vec::new();
    let mut cut = false;
    let mut used = 0usize;
    let mut first = true;
    let mut token = lexer.next_token();
    loop {
        match token {
            Token::Newline | Token::End => return (PhraseEnd::Newline, out),
            t if t == stop => return (PhraseEnd::Stop, out),
            _ => {}
        }
        if used + lexer.text.len() + 1 >= PHRASE_LIMIT - 1 {
            return (PhraseEnd::Overflow, out);
        }
        if !first {
            if !cut {
                out.push(b' ');
            }
            used += 1;
        }
        if !cut {
            let visible = c_str(&lexer.text);
            out.extend_from_slice(visible);
            cut = visible.len() < lexer.text.len();
        }
        first = false;
        used += lexer.text.len();
        token = lexer.next_token();
    }
}

/// `FIniLexer`. `current` is the byte last read as a signed char, or -1 at
/// the end; a 0xFF byte therefore ends the input, as in the C++. A word may
/// be any length; the C++ overruns its buffer past 1024 bytes (divergence
/// D2).
struct Lexer<'a> {
    source: &'a [u8],
    cursor: usize,
    current: i32,
    text: Vec<u8>,
}

impl<'a> Lexer<'a> {
    fn new(source: &'a [u8]) -> Self {
        Self {
            source,
            cursor: 0,
            current: i32::from(b'\n'),
            text: Vec::new(),
        }
    }

    fn advance(&mut self) {
        self.current = match self.source.get(self.cursor) {
            Some(&byte) => {
                self.cursor += 1;
                i32::from(i8::from_ne_bytes([byte]))
            }
            None => -1,
        };
    }

    fn current_byte(&self) -> u8 {
        self.current.to_ne_bytes()[0]
    }

    fn single(&mut self, token: Token) -> Token {
        self.text.push(self.current_byte());
        self.advance();
        token
    }

    fn next_token(&mut self) -> Token {
        self.text.clear();
        loop {
            match self.current {
                -1 => return Token::End,
                0x09 | 0x0D | 0x20 => self.advance(),
                0x3B => {
                    // A comment runs to the newline, which stays current.
                    loop {
                        self.advance();
                        if self.current == i32::from(b'\n') || self.current == -1 {
                            break;
                        }
                    }
                }
                0x0A => return self.single(Token::Newline),
                0x5B => return self.single(Token::OpenBracket),
                0x5D => return self.single(Token::CloseBracket),
                0x3D => return self.single(Token::Equals),
                _ => {
                    loop {
                        self.text.push(self.current_byte());
                        self.advance();
                        let delimiter = self.current == 0
                            || matches!(
                                self.current,
                                0x20 | 0x0D | 0x0A | 0x09 | 0x5B | 0x5D | 0x3B | 0x3D
                            );
                        if delimiter || self.current == -1 {
                            break;
                        }
                    }
                    return if eq_ignore_case(&self.text, b"@include") {
                        Token::Include
                    } else {
                        Token::Word
                    };
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(source: &[u8]) -> (EngineIni, Result<(), EngineIniError>) {
        let mut ini = EngineIni::new();
        let result = ini.load_buffer(source, &mut NoIncludes);
        (ini, result)
    }

    #[test]
    fn keys_values_and_spacing() {
        let (ini, result) = parse(b"[ Render  Options ]\nkey one = a=b  c ; note\nflag\nempty =\n");
        assert_eq!(result, Ok(()));
        let section = ini.section(b"render options").expect("section");
        assert_eq!(section.line(0).as_deref(), Some(&b"key one = a = b c"[..]));
        assert_eq!(section.line(1).as_deref(), Some(&b"flag"[..]));
        assert_eq!(section.line(2).as_deref(), Some(&b"empty = "[..]));
        assert_eq!(section.value(b"FLAG"), Some(&b""[..]));
        assert_eq!(section.value(b"missing"), None);
    }

    #[test]
    fn sections_sort_and_duplicates_follow_their_equals() {
        let (ini, _) = parse(b"[b]\nx=1\n[A]\n[b]\nx=2\n[_]\n");
        let names: Vec<&[u8]> = ini.sections().iter().map(|s| &s.name[..]).collect();
        assert_eq!(names, [&b"_"[..], b"A", b"b", b"b"]);
        assert_eq!(
            ini.section(b"B").and_then(|s| s.value(b"x")),
            Some(&b"1"[..])
        );
    }

    #[test]
    fn errors_keep_what_was_parsed() {
        let (ini, result) = parse(b"[s]\na=1\n]\nb=2\n");
        assert_eq!(result, Err(EngineIniError::Breakpoint));
        assert_eq!(ini.sections()[0].entries.len(), 1);
        assert_eq!(parse(b"a=1\n").1, Err(EngineIniError::KeyBeforeSection));
        assert_eq!(
            parse(b"[s]\n@include\n").1,
            Err(EngineIniError::IncludeWithoutName)
        );
    }

    #[test]
    fn a_0xff_byte_ends_the_input() {
        let (ini, result) = parse(b"[s]\na=1\xFFb=2\n");
        assert_eq!(result, Ok(()));
        assert_eq!(ini.sections()[0].line(0).as_deref(), Some(&b"a = 1"[..]));
        assert_eq!(ini.sections()[0].entries.len(), 1);
    }

    #[test]
    fn includes_resolve_next_to_the_including_file() {
        let mut seen = Vec::new();
        let mut loader = |path: &[u8]| {
            seen.push(path.to_vec());
            (path == b"cfg\\sub\\more.ini").then(|| b"y=2\n".to_vec())
        };
        let mut ini = EngineIni::new();
        let result = ini.load_named(
            b"cfg\\main.ini",
            b"[s]\nx=1\n@include sub\\more.ini\nz=3\n",
            &mut loader,
        );
        assert_eq!(result, Ok(()));
        assert_eq!(seen, [b"cfg\\sub\\more.ini".to_vec()]);
        let keys: Vec<&[u8]> = ini.sections()[0]
            .entries
            .iter()
            .map(|e| &e.key[..])
            .collect();
        assert_eq!(keys, [&b"x"[..], b"y", b"z"]);
        assert_eq!(
            include_path(b"\\abs\\a.ini", b"c:\\x\\y.ini"),
            b"\\abs\\a.ini"
        );
        assert_eq!(include_path(b"d:a.ini", b"c:\\x\\y.ini"), b"d:a.ini");
        assert_eq!(include_path(b"a.ini", b"c:\\x\\y.ini"), b"c:\\x\\a.ini");
    }

    #[test]
    fn include_cycles_stop() {
        let mut loader = |_: &[u8]| Some(b"@include self.ini\n".to_vec());
        let mut ini = EngineIni::new();
        let result = ini.load_named(b"self.ini", b"@include self.ini\n", &mut loader);
        assert_eq!(result, Err(EngineIniError::IncludeTooDeep));
    }

    #[test]
    fn long_phrases_overflow() {
        let mut source = b"[s]\nk = ".to_vec();
        source.extend(std::iter::repeat_n(b'v', 0x3FD));
        assert_eq!(parse(&source).1, Ok(()));
        source.push(b'v');
        assert_eq!(parse(&source).1, Err(EngineIniError::Breakpoint));
    }
}
