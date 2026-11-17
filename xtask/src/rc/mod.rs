//! A resource-script reader for the text catalogs (work package P1.6). It
//! reads what `rc.exe` would store for the parts the game looks up:
//! `STRINGTABLE` entries and resources of type 23 (`HTML`) taken from a
//! file. Every other resource (dialogs, menus, accelerators, version
//! information, icons, bitmaps) is parsed only far enough to skip it.
//!
//! The rules follow `rc.exe`: keywords in any case, `{`/`}` or
//! `BEGIN`/`END`, an optional comma after a string ID, adjacent string
//! literals joined, the escapes of [`string::process`], the preprocessor of
//! [`pp`], and language 0x0409 until a `LANGUAGE` statement. Constructs the
//! game's scripts do not use and whose `rc.exe` result is not certain are
//! errors rather than guesses.

mod lex;
mod pp;
mod string;

use std::fmt;
use std::path::{Path, PathBuf};

pub use lex::Location;
use lex::Tok;
use pp::{PToken, Preprocessor};
pub use string::CodePage;

/// The resource type the game's rich-text cards use (`RT_HTML`).
const CARD_TYPE: i64 = 23;
/// `rc.exe` without `/l`: `LANG_ENGLISH`, `SUBLANG_ENGLISH_US`.
const DEFAULT_LANGUAGE: u16 = 0x0409;

/// Symbols the game's scripts take from `windows.h` (`winnt.h`).
const SYMBOLS: &[(&str, i64)] = &[
    ("LANG_NEUTRAL", 0x00),
    ("LANG_ENGLISH", 0x09),
    ("SUBLANG_NEUTRAL", 0x00),
    ("SUBLANG_DEFAULT", 0x01),
    ("SUBLANG_SYS_DEFAULT", 0x02),
    ("SUBLANG_ENGLISH_US", 0x01),
    ("SUBLANG_ENGLISH_UK", 0x02),
];

/// Memory flags a resource header may carry; they do not change the data.
const MEMORY_FLAGS: &[&str] = &[
    "PRELOAD",
    "LOADONCALL",
    "FIXED",
    "MOVEABLE",
    "DISCARDABLE",
    "PURE",
    "IMPURE",
    "SHARED",
    "NONSHARED",
];

/// Resource types whose definition always ends in a block.
const BLOCK_TYPES: &[&str] = &[
    "DIALOG",
    "DIALOGEX",
    "MENU",
    "MENUEX",
    "ACCELERATORS",
    "VERSIONINFO",
    "TOOLBAR",
    "DLGINIT",
    "TEXTINCLUDE",
    "DESIGNINFO",
];

/// A problem in a script, with its place.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RcError {
    pub file: PathBuf,
    pub line: u32,
    pub message: String,
}

impl RcError {
    pub fn at(file: &Path, line: u32, message: impl Into<String>) -> Self {
        Self {
            file: file.to_path_buf(),
            line,
            message: message.into(),
        }
    }
}

impl fmt::Display for RcError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: {}", self.file.display(), self.line, self.message)
    }
}

/// How a script is compiled.
#[derive(Clone, Debug)]
pub struct RcOptions {
    /// The code page at the start (`rc /c`).
    pub code_page: CodePage,
    /// Folders searched after the script's own folder (`rc /i`).
    pub include_dirs: Vec<PathBuf>,
}

/// One `STRINGTABLE` entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RcString {
    pub id: u16,
    pub language: u16,
    pub units: Vec<u16>,
    pub loc: Location,
}

/// One type-23 resource taken from a file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RcCard {
    pub id: u16,
    pub language: u16,
    pub path: PathBuf,
    pub loc: Location,
}

/// The string-table entries and cards of a script, in source order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RcScript {
    pub strings: Vec<RcString>,
    pub cards: Vec<RcCard>,
}

/// Reads the script at `path`.
pub fn parse_file(path: &Path, options: &RcOptions) -> Result<RcScript, RcError> {
    let mut pp = Preprocessor::new(options.code_page, &options.include_dirs);
    pp.file(path, false)?;
    let end = pp.out.last().map_or_else(
        || Location {
            file: std::rc::Rc::from(path),
            line: 1,
        },
        |t| t.loc.clone(),
    );
    let mut parser = Parser {
        toks: pp.out,
        pos: 0,
        language: DEFAULT_LANGUAGE,
        include_dirs: &options.include_dirs,
        end,
        script: RcScript::default(),
    };
    parser.script()?;
    Ok(parser.script)
}

/// The value of a number token: decimal or `0x` hex, with an optional
/// `L`/`U` suffix. A leading zero is an error (the octal rule of `rc.exe`
/// is not certain and the game's scripts do not use it).
fn number(text: &str, loc: &Location) -> Result<i64, RcError> {
    let digits = text.trim_end_matches(['L', 'l', 'U', 'u']);
    let value = if let Some(hex) = digits
        .strip_prefix("0x")
        .or_else(|| digits.strip_prefix("0X"))
    {
        u32::from_str_radix(hex, 16).ok()
    } else if digits.len() > 1 && digits.starts_with('0') {
        return Err(loc.error(format!("number `{text}` has a leading zero")));
    } else {
        digits.parse::<u32>().ok()
    };
    value
        .map(i64::from)
        .ok_or_else(|| loc.error(format!("invalid number `{text}`")))
}

fn is_block_open(t: &PToken) -> bool {
    t.is_punct(b'{') || t.is_keyword("BEGIN")
}

fn is_block_close(t: &PToken) -> bool {
    t.is_punct(b'}') || t.is_keyword("END")
}

struct Parser<'a> {
    toks: Vec<PToken>,
    pos: usize,
    language: u16,
    include_dirs: &'a [PathBuf],
    end: Location,
    script: RcScript,
}

impl Parser<'_> {
    fn peek(&self) -> Option<&PToken> {
        self.toks.get(self.pos)
    }

    fn next(&mut self) -> Result<PToken, RcError> {
        let token = self
            .toks
            .get(self.pos)
            .cloned()
            .ok_or_else(|| self.end.error("unexpected end of file"))?;
        self.pos += 1;
        Ok(token)
    }

    fn script(&mut self) -> Result<(), RcError> {
        while let Some(t) = self.peek() {
            if t.is_keyword("LANGUAGE") {
                self.pos += 1;
                self.language = self.language_args()?;
            } else if t.is_keyword("STRINGTABLE") {
                self.stringtable()?;
            } else if t.is_keyword("CHARACTERISTICS") || t.is_keyword("VERSION") {
                self.pos += 1;
                self.expr()?;
            } else {
                self.resource()?;
            }
        }
        Ok(())
    }

    /// `primary, sub` of a `LANGUAGE` statement, as a language ID.
    fn language_args(&mut self) -> Result<u16, RcError> {
        let loc = self.next_loc();
        let primary = self.expr()?;
        let comma = self.next()?;
        if !comma.is_punct(b',') {
            return Err(comma.loc.error("expected `,` in LANGUAGE"));
        }
        let sub = self.expr()?;
        if !(0..=0x3FF).contains(&primary) || !(0..=0x3F).contains(&sub) {
            return Err(loc.error("LANGUAGE value out of range"));
        }
        u16::try_from((sub << 10) | primary).map_err(|_| loc.error("LANGUAGE value out of range"))
    }

    fn next_loc(&self) -> Location {
        self.peek()
            .map_or_else(|| self.end.clone(), |t| t.loc.clone())
    }

    /// An integer expression: numbers, the built-in symbols, unary `-` and
    /// `~`, parentheses, and `+ - | &` from left to right.
    fn expr(&mut self) -> Result<i64, RcError> {
        let mut value = self.unary()?;
        while let Some(op) = self.peek().and_then(|t| match t.tok {
            Tok::Punct(ch @ (b'+' | b'-' | b'|' | b'&')) => Some(ch),
            _ => None,
        }) {
            self.pos += 1;
            let right = self.unary()?;
            value = match op {
                b'+' => value.wrapping_add(right),
                b'-' => value.wrapping_sub(right),
                b'|' => value | right,
                _ => value & right,
            };
        }
        Ok(value)
    }

    fn unary(&mut self) -> Result<i64, RcError> {
        let t = self.next()?;
        match &t.tok {
            Tok::Punct(b'-') => Ok(self.unary()?.wrapping_neg()),
            Tok::Punct(b'~') => Ok(!self.unary()?),
            Tok::Punct(b'(') => {
                let value = self.expr()?;
                let close = self.next()?;
                if close.is_punct(b')') {
                    Ok(value)
                } else {
                    Err(close.loc.error("expected `)`"))
                }
            }
            Tok::Number(text) => number(text, &t.loc),
            Tok::Ident(name) => SYMBOLS
                .iter()
                .find(|(symbol, _)| symbol == name)
                .map(|&(_, value)| value)
                .ok_or_else(|| t.loc.error(format!("unknown symbol `{name}`"))),
            _ => Err(t
                .loc
                .error(format!("expected a number, found {}", t.describe()))),
        }
    }

    /// Memory flags and the `LANGUAGE`, `CHARACTERISTICS` and `VERSION`
    /// statements of a resource header.
    fn header_options(&mut self, language: &mut u16) -> Result<(), RcError> {
        while let Some(t) = self.peek() {
            if MEMORY_FLAGS.iter().any(|flag| t.is_keyword(flag)) {
                self.pos += 1;
            } else if t.is_keyword("LANGUAGE") {
                self.pos += 1;
                *language = self.language_args()?;
            } else if t.is_keyword("CHARACTERISTICS") || t.is_keyword("VERSION") {
                self.pos += 1;
                self.expr()?;
            } else {
                break;
            }
        }
        Ok(())
    }

    /// Skips a block whose opening token is next.
    fn skip_block(&mut self) -> Result<(), RcError> {
        let mut depth = 0usize;
        loop {
            let t = self.next()?;
            if is_block_open(&t) {
                depth += 1;
            } else if is_block_close(&t) {
                depth = depth.saturating_sub(1);
                if depth == 0 {
                    return Ok(());
                }
            } else if depth == 0 {
                return Err(t.loc.error("expected `{` or BEGIN"));
            }
        }
    }

    fn stringtable(&mut self) -> Result<(), RcError> {
        let keyword = self.next()?;
        let mut language = self.language;
        self.header_options(&mut language)?;
        let open = self.next()?;
        if !is_block_open(&open) {
            return Err(open.loc.error("expected `{` or BEGIN after STRINGTABLE"));
        }
        loop {
            let t = self
                .peek()
                .ok_or_else(|| keyword.loc.error("STRINGTABLE without its end"))?;
            if is_block_close(t) {
                self.pos += 1;
                return Ok(());
            }
            let loc = t.loc.clone();
            let id = self.expr()?;
            if self.peek().is_some_and(|t| t.is_punct(b',')) {
                self.pos += 1;
            }
            let mut units = Vec::new();
            let mut parts = 0;
            while let Some(PToken {
                tok: Tok::Str { wide, raw },
                loc: part_loc,
                code_page,
            }) = self.peek()
            {
                let processed =
                    string::process(raw, *wide, *code_page).map_err(|m| part_loc.error(m))?;
                units.extend(processed);
                parts += 1;
                self.pos += 1;
            }
            if parts == 0 {
                return Err(loc.error("expected a string after the string ID"));
            }
            let id = u16::try_from(id)
                .map_err(|_| loc.error(format!("string ID {id} is outside 0..=65535")))?;
            self.script.strings.push(RcString {
                id,
                language,
                units,
                loc,
            });
        }
    }

    fn resource(&mut self) -> Result<(), RcError> {
        let name = self.next()?;
        let numeric_name = match &name.tok {
            Tok::Number(text) => Some(number(text, &name.loc)?),
            Tok::Ident(_) | Tok::Str { .. } => None,
            Tok::Punct(_) => {
                return Err(name.loc.error(format!("unexpected {}", name.describe())));
            }
        };
        let kind = self.next()?;
        if BLOCK_TYPES.iter().any(|word| kind.is_keyword(word)) {
            while self.peek().is_some_and(|t| !is_block_open(t)) {
                self.pos += 1;
            }
            return self.skip_block();
        }
        let is_card = match &kind.tok {
            Tok::Number(text) => number(text, &kind.loc)? == CARD_TYPE,
            Tok::Ident(word) => word.eq_ignore_ascii_case("HTML"),
            Tok::Str { .. } => false,
            Tok::Punct(_) => {
                return Err(kind.loc.error(format!(
                    "expected a resource type, found {}",
                    kind.describe()
                )));
            }
        };
        let mut language = self.language;
        self.header_options(&mut language)?;
        let body = self.next()?;
        match &body.tok {
            Tok::Str { wide, raw } => {
                if !is_card {
                    return Ok(());
                }
                let id = numeric_name
                    .and_then(|n| u16::try_from(n).ok())
                    .ok_or_else(|| {
                        name.loc
                            .error("a type-23 resource needs a numeric ID up to 65535")
                    })?;
                let file_name =
                    string::process(raw, *wide, body.code_page).map_err(|m| body.loc.error(m))?;
                let file_name = String::from_utf16(&file_name)
                    .map_err(|_| body.loc.error("file name is not valid UTF-16"))?;
                let path = pp::resolve(&body.loc.file, &file_name, self.include_dirs)
                    .ok_or_else(|| body.loc.error(format!("\"{file_name}\": file not found")))?;
                self.script.cards.push(RcCard {
                    id,
                    language,
                    path,
                    loc: name.loc,
                });
                Ok(())
            }
            _ if is_block_open(&body) => {
                if is_card {
                    return Err(body
                        .loc
                        .error("type-23 data written inline is not supported"));
                }
                self.pos -= 1;
                self.skip_block()
            }
            _ => Err(body.loc.error(format!(
                "expected a file name or a block, found {}",
                body.describe()
            ))),
        }
    }
}

#[cfg(test)]
mod tests;
