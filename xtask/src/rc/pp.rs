//! The preprocessor part of `rc.exe` that resource scripts use: `#include`,
//! object-like `#define` and `#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`
//! and `#pragma code_page`. Anything else is reported, not guessed.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use super::RcError;
use super::lex::{Location, Tok, Token, source_text, tokenize};
use super::string::CodePage;

/// `#include` files nest at most this deep.
const MAX_INCLUDE_DEPTH: usize = 32;

/// A token after preprocessing, with the code page in effect where it
/// appeared.
#[derive(Clone, Debug)]
pub struct PToken {
    pub tok: Tok,
    pub loc: Location,
    pub code_page: CodePage,
}

impl PToken {
    pub fn is_punct(&self, ch: u8) -> bool {
        self.tok == Tok::Punct(ch)
    }

    pub fn is_keyword(&self, word: &str) -> bool {
        matches!(&self.tok, Tok::Ident(name) if name.eq_ignore_ascii_case(word))
    }

    pub fn describe(&self) -> String {
        Token {
            tok: self.tok.clone(),
            loc: self.loc.clone(),
            line_start: false,
            space_before: false,
        }
        .describe()
    }
}

struct Conditional {
    active: bool,
    parent_active: bool,
    taken: bool,
    seen_else: bool,
}

pub struct Preprocessor<'a> {
    include_dirs: &'a [PathBuf],
    macros: HashMap<String, Vec<Tok>>,
    code_page: CodePage,
    /// Inside a UTF-16 script, which is read as Unicode whatever the code
    /// page.
    in_utf16: bool,
    depth: usize,
    pub out: Vec<PToken>,
}

impl<'a> Preprocessor<'a> {
    pub fn new(code_page: CodePage, include_dirs: &'a [PathBuf]) -> Self {
        // rc.exe defines these.
        let one = vec![Tok::Number("1".to_owned())];
        let macros = HashMap::from([
            ("RC_INVOKED".to_owned(), one.clone()),
            ("_WIN32".to_owned(), one),
        ]);
        Self {
            include_dirs,
            macros,
            code_page,
            in_utf16: false,
            depth: 0,
            out: Vec::new(),
        }
    }

    /// Preprocesses `path`. In a `.h` or `.c` file only the directives
    /// count, as in `rc.exe`.
    pub fn file(&mut self, path: &Path, directives_only: bool) -> Result<(), RcError> {
        let bytes =
            std::fs::read(path).map_err(|e| RcError::at(path, 0, format!("cannot read: {e}")))?;
        let text = source_text(bytes, path)?;
        let saved = (self.code_page, self.in_utf16);
        if text.was_utf16 {
            self.code_page = CodePage::Utf8;
            self.in_utf16 = true;
        }
        let file: Rc<Path> = Rc::from(path);
        let tokens = tokenize(&text.bytes, &file)?;
        let mut conditionals: Vec<Conditional> = Vec::new();
        let mut i = 0;
        while i < tokens.len() {
            let token = &tokens[i];
            if token.line_start && token.is_punct(b'#') {
                let end = tokens[i + 1..]
                    .iter()
                    .position(|t| t.line_start)
                    .map_or(tokens.len(), |p| i + 1 + p);
                self.directive(&tokens[i + 1..end], &token.loc, &mut conditionals)?;
                i = end;
                continue;
            }
            let active = conditionals.last().is_none_or(|c| c.active);
            if active && !directives_only {
                let mut expanding = Vec::new();
                self.emit(token.tok.clone(), &token.loc, &mut expanding);
            }
            i += 1;
        }
        if !conditionals.is_empty() {
            let line = tokens.last().map_or(1, |t| t.loc.line);
            return Err(RcError::at(path, line, "#ifdef or #ifndef without #endif"));
        }
        if text.was_utf16 {
            (self.code_page, self.in_utf16) = saved;
        }
        Ok(())
    }

    fn emit(&mut self, tok: Tok, loc: &Location, expanding: &mut Vec<String>) {
        if let Tok::Ident(name) = &tok
            && !expanding.contains(name)
            && let Some(body) = self.macros.get(name).cloned()
        {
            expanding.push(name.clone());
            for part in body {
                self.emit(part, loc, expanding);
            }
            expanding.pop();
            return;
        }
        self.out.push(PToken {
            tok,
            loc: loc.clone(),
            code_page: self.code_page,
        });
    }

    fn directive(
        &mut self,
        toks: &[Token],
        hash: &Location,
        conditionals: &mut Vec<Conditional>,
    ) -> Result<(), RcError> {
        let active = conditionals.last().is_none_or(|c| c.active);
        let Some(first) = toks.first() else {
            return Ok(());
        };
        let Tok::Ident(name) = &first.tok else {
            if !active {
                return Ok(());
            }
            return Err(hash.error(format!("invalid directive {}", first.describe())));
        };
        let ident = |at: usize| match toks.get(at).map(|t| &t.tok) {
            Some(Tok::Ident(word)) => Ok(word.clone()),
            _ => Err(hash.error(format!("#{name} needs a name"))),
        };
        match name.as_str() {
            "ifdef" | "ifndef" => {
                let taken = if active {
                    let defined = self.macros.contains_key(&ident(1)?);
                    defined == (name == "ifdef")
                } else {
                    false
                };
                conditionals.push(Conditional {
                    active: active && taken,
                    parent_active: active,
                    taken,
                    seen_else: false,
                });
            }
            "if" => {
                if active {
                    return Err(hash.error("#if is not supported"));
                }
                conditionals.push(Conditional {
                    active: false,
                    parent_active: false,
                    taken: true,
                    seen_else: false,
                });
            }
            "elif" => match conditionals.last() {
                Some(c) if !c.parent_active => {}
                Some(_) => return Err(hash.error("#elif is not supported")),
                None => return Err(hash.error("#elif without #if")),
            },
            "else" => {
                let c = conditionals
                    .last_mut()
                    .ok_or_else(|| hash.error("#else without #ifdef"))?;
                if c.seen_else {
                    return Err(hash.error("second #else"));
                }
                c.seen_else = true;
                c.active = c.parent_active && !c.taken;
            }
            "endif" => {
                conditionals
                    .pop()
                    .ok_or_else(|| hash.error("#endif without #ifdef"))?;
            }
            _ if !active => {}
            "include" => self.include(toks, hash)?,
            "define" => {
                let macro_name = ident(1)?;
                if toks
                    .get(2)
                    .is_some_and(|t| t.is_punct(b'(') && !t.space_before)
                {
                    return Err(
                        hash.error(format!("function-like macro {macro_name} is not supported"))
                    );
                }
                let body = toks[2..].iter().map(|t| t.tok.clone()).collect();
                self.macros.insert(macro_name, body);
            }
            "undef" => {
                self.macros.remove(&ident(1)?);
            }
            "pragma" => match ident(1)?.as_str() {
                "code_page" if self.in_utf16 => {
                    return Err(hash.error("#pragma code_page in a UTF-16 script is not supported"));
                }
                "code_page" => {
                    let number = match (toks.get(2), toks.get(3), toks.get(4)) {
                        (Some(open), Some(value), Some(close))
                            if open.is_punct(b'(') && close.is_punct(b')') =>
                        {
                            match &value.tok {
                                Tok::Number(text) => text.parse::<u32>().ok(),
                                _ => None,
                            }
                        }
                        _ => None,
                    };
                    self.code_page = number.and_then(CodePage::from_number).ok_or_else(|| {
                        hash.error("#pragma code_page supports only 65001 and 1252")
                    })?;
                }
                "once" => {}
                other => return Err(hash.error(format!("#pragma {other} is not supported"))),
            },
            "error" => return Err(hash.error("#error")),
            other => return Err(hash.error(format!("#{other} is not supported"))),
        }
        Ok(())
    }

    fn include(&mut self, toks: &[Token], hash: &Location) -> Result<(), RcError> {
        match toks.get(1).map(|t| &t.tok) {
            // A system header (windows.h, commctrl.h). The symbols the
            // scripts take from it are built into the parser.
            Some(Tok::Punct(b'<')) => Ok(()),
            Some(Tok::Str { wide: false, raw }) => {
                let name = String::from_utf8(raw.clone())
                    .map_err(|_| hash.error("#include name is not UTF-8"))?;
                let path = resolve(&hash.file, &name, self.include_dirs)
                    .ok_or_else(|| hash.error(format!("#include \"{name}\": file not found")))?;
                if self.depth >= MAX_INCLUDE_DEPTH {
                    return Err(hash.error("#include files nested too deep"));
                }
                let directives_only = path.extension().is_some_and(|ext| {
                    ext.eq_ignore_ascii_case("h") || ext.eq_ignore_ascii_case("c")
                });
                self.depth += 1;
                let result = self.file(&path, directives_only);
                self.depth -= 1;
                result
            }
            _ => Err(hash.error("#include needs \"file\" or <file>")),
        }
    }
}

/// A file named in a script: relative to the script's folder, then to each
/// include folder. Both separators are accepted and empty components
/// dropped.
pub fn resolve(from: &Path, name: &str, include_dirs: &[PathBuf]) -> Option<PathBuf> {
    let relative: PathBuf = name
        .split(['\\', '/'])
        .filter(|part| !part.is_empty())
        .collect();
    if relative.as_os_str().is_empty() {
        return None;
    }
    let absolute = name.starts_with(['\\', '/']) || name.get(1..2) == Some(":");
    if absolute {
        let path = PathBuf::from(name);
        return path.is_file().then_some(path);
    }
    from.parent()
        .into_iter()
        .chain(include_dirs.iter().map(PathBuf::as_path))
        .map(|dir| dir.join(&relative))
        .find(|candidate| candidate.is_file())
}
