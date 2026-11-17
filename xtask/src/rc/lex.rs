//! Source reading, line splicing and tokens of a resource script.

use std::path::Path;
use std::rc::Rc;

use super::RcError;

/// Where a token starts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Location {
    pub file: Rc<Path>,
    pub line: u32,
}

impl Location {
    pub fn error(&self, message: impl Into<String>) -> RcError {
        RcError {
            file: self.file.to_path_buf(),
            line: self.line,
            message: message.into(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Tok {
    Ident(String),
    /// A number as written, suffix included.
    Number(String),
    /// A string literal: the bytes between the quotes, `""` still doubled
    /// and escapes not processed. `wide` for `L"..."`.
    Str {
        wide: bool,
        raw: Vec<u8>,
    },
    Punct(u8),
}

#[derive(Clone, Debug)]
pub struct Token {
    pub tok: Tok,
    pub loc: Location,
    /// The first token of a line (after splicing).
    pub line_start: bool,
    /// Whitespace or a comment separates it from the token before.
    pub space_before: bool,
}

impl Token {
    pub fn is_punct(&self, ch: u8) -> bool {
        self.tok == Tok::Punct(ch)
    }

    pub fn describe(&self) -> String {
        match &self.tok {
            Tok::Ident(name) => format!("`{name}`"),
            Tok::Number(text) => format!("number `{text}`"),
            Tok::Str { .. } => "a string".to_owned(),
            Tok::Punct(ch) => format!("`{}`", char::from(*ch)),
        }
    }
}

/// The text of a source file as bytes, and whether it was UTF-16 (then it
/// is converted to UTF-8 and read as code page 65001).
pub struct SourceText {
    pub bytes: Vec<u8>,
    pub was_utf16: bool,
}

/// Decodes a UTF-16LE file that starts with a byte-order mark and drops a
/// UTF-8 byte-order mark; other files are returned as they are.
pub fn source_text(bytes: Vec<u8>, file: &Path) -> Result<SourceText, RcError> {
    if let Some(rest) = bytes.strip_prefix(&[0xFF, 0xFE]) {
        if rest.len() % 2 != 0 {
            return Err(RcError::at(file, 1, "odd length in a UTF-16 file"));
        }
        let units: Vec<u16> = rest
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&pair| u16::from_le_bytes(pair))
            .collect();
        let text = String::from_utf16(&units)
            .map_err(|_| RcError::at(file, 1, "invalid UTF-16 in the file"))?;
        return Ok(SourceText {
            bytes: text.into_bytes(),
            was_utf16: true,
        });
    }
    let bytes = match bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]) {
        Some(rest) => rest.to_vec(),
        None => bytes,
    };
    Ok(SourceText {
        bytes,
        was_utf16: false,
    })
}

/// Splices lines (a backslash before a line end removes both, translation
/// phase 2 of C) and turns CR LF into LF. Each byte keeps its line number.
fn splice(bytes: &[u8]) -> (Vec<u8>, Vec<u32>) {
    let mut out = Vec::with_capacity(bytes.len());
    let mut lines = Vec::with_capacity(bytes.len());
    let mut line = 1u32;
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        if b == b'\\' {
            if bytes.get(i + 1) == Some(&b'\n') {
                i += 2;
                line += 1;
                continue;
            }
            if bytes.get(i + 1) == Some(&b'\r') && bytes.get(i + 2) == Some(&b'\n') {
                i += 3;
                line += 1;
                continue;
            }
        }
        if b == b'\r' && bytes.get(i + 1) == Some(&b'\n') {
            i += 1;
            continue;
        }
        out.push(b);
        lines.push(line);
        if b == b'\n' {
            line += 1;
        }
        i += 1;
    }
    (out, lines)
}

fn is_ident_start(b: u8) -> bool {
    b.is_ascii_alphabetic() || b == b'_'
}

fn is_ident_continue(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// The tokens of a source file.
pub fn tokenize(bytes: &[u8], file: &Rc<Path>) -> Result<Vec<Token>, RcError> {
    let (src, lines) = splice(bytes);
    let line_of = |i: usize| {
        lines
            .get(i)
            .copied()
            .unwrap_or_else(|| lines.last().copied().unwrap_or(1))
    };
    let loc = |i: usize| Location {
        file: Rc::clone(file),
        line: line_of(i),
    };
    let mut tokens = Vec::new();
    let mut line_start = true;
    let mut space_before = true;
    let mut i = 0;
    while i < src.len() {
        let b = src[i];
        match b {
            b'\n' => {
                line_start = true;
                space_before = true;
                i += 1;
            }
            b' ' | b'\t' | b'\r' | 0x0B | 0x0C => {
                space_before = true;
                i += 1;
            }
            b'/' if src.get(i + 1) == Some(&b'/') => {
                while i < src.len() && src[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if src.get(i + 1) == Some(&b'*') => {
                let start = i;
                i += 2;
                loop {
                    if i + 1 >= src.len() {
                        return Err(loc(start).error("unterminated comment"));
                    }
                    if src[i] == b'*' && src[i + 1] == b'/' {
                        i += 2;
                        break;
                    }
                    i += 1;
                }
                space_before = true;
            }
            b'"' => {
                let (raw, end) =
                    string_literal(&src, i).ok_or_else(|| loc(i).error("unterminated string"))?;
                tokens.push(Token {
                    tok: Tok::Str { wide: false, raw },
                    loc: loc(i),
                    line_start,
                    space_before,
                });
                i = end;
                line_start = false;
                space_before = false;
            }
            _ if is_ident_start(b) => {
                let start = i;
                while i < src.len() && is_ident_continue(src[i]) {
                    i += 1;
                }
                let word = String::from_utf8_lossy(&src[start..i]).into_owned();
                let tok = if (word == "L" || word == "l") && src.get(i) == Some(&b'"') {
                    let (raw, end) = string_literal(&src, i)
                        .ok_or_else(|| loc(i).error("unterminated string"))?;
                    i = end;
                    Tok::Str { wide: true, raw }
                } else {
                    Tok::Ident(word)
                };
                tokens.push(Token {
                    tok,
                    loc: loc(start),
                    line_start,
                    space_before,
                });
                line_start = false;
                space_before = false;
            }
            _ if b.is_ascii_digit() => {
                let start = i;
                while i < src.len() && is_ident_continue(src[i]) {
                    i += 1;
                }
                tokens.push(Token {
                    tok: Tok::Number(String::from_utf8_lossy(&src[start..i]).into_owned()),
                    loc: loc(start),
                    line_start,
                    space_before,
                });
                line_start = false;
                space_before = false;
            }
            b'\'' => return Err(loc(i).error("character literals are not supported")),
            _ if b.is_ascii_graphic() => {
                tokens.push(Token {
                    tok: Tok::Punct(b),
                    loc: loc(i),
                    line_start,
                    space_before,
                });
                i += 1;
                line_start = false;
                space_before = false;
            }
            _ => {
                return Err(loc(i).error(format!(
                    "unexpected byte 0x{b:02x} outside a string or comment"
                )));
            }
        }
    }
    Ok(tokens)
}

/// The content of the string literal whose opening quote is at `open`, and
/// the index after its closing quote. `""` is part of the content; a
/// backslash does not end or escape anything here (as in `rc.exe`, which
/// takes `\"` only before another quote, see `string::process`).
fn string_literal(src: &[u8], open: usize) -> Option<(Vec<u8>, usize)> {
    let mut i = open + 1;
    while i < src.len() {
        match src[i] {
            b'"' if src.get(i + 1) == Some(&b'"') => i += 2,
            b'"' => return Some((src[open + 1..i].to_vec(), i + 1)),
            b'\n' => return None,
            _ => i += 1,
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn toks(text: &str) -> Vec<Tok> {
        let file: Rc<Path> = Rc::from(Path::new("t.rc"));
        tokenize(text.as_bytes(), &file)
            .unwrap()
            .into_iter()
            .map(|t| t.tok)
            .collect()
    }

    #[test]
    fn splices_and_skips_comments() {
        assert_eq!(
            toks("1 /* x\n y */ \"a//b\" // c\n0x1fL L\"w\"\\\nNAME{"),
            vec![
                Tok::Number("1".into()),
                Tok::Str {
                    wide: false,
                    raw: b"a//b".to_vec()
                },
                Tok::Number("0x1fL".into()),
                Tok::Str {
                    wide: true,
                    raw: b"w".to_vec()
                },
                Tok::Ident("NAME".into()),
                Tok::Punct(b'{'),
            ]
        );
    }

    #[test]
    fn doubled_quotes_stay_in_the_literal() {
        assert_eq!(
            toks(r#""say ""hi"" \"" x""#),
            vec![Tok::Str {
                wide: false,
                raw: br#"say ""hi"" \"" x"#.to_vec()
            },]
        );
    }

    #[test]
    fn line_numbers_follow_splices() {
        let file: Rc<Path> = Rc::from(Path::new("t.rc"));
        let tokens = tokenize(b"a\\\nb\r\nc\n", &file).unwrap();
        let lines: Vec<(u32, bool)> = tokens.iter().map(|t| (t.loc.line, t.line_start)).collect();
        assert_eq!(lines, [(1, true), (3, true)]);
        assert_eq!(tokens[0].tok, Tok::Ident("ab".into()));
    }

    #[test]
    fn unterminated_string_is_an_error() {
        let file: Rc<Path> = Rc::from(Path::new("t.rc"));
        let error = tokenize(b"1 \"abc\n\"", &file).unwrap_err();
        assert_eq!(error.line, 1);
    }
}
