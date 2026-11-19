//! A Lua 3.2 compiler for the subset `cargo xtask thn-decompile` and the
//! scene writer write, so a decompiled script can be checked against its
//! chunk byte for byte and a saved scene reloaded.
//!
//! The code generation follows `lparser.c` in `CppSource/luaprofile/lua32/
//! src` function by function: the same instruction forms, constant table
//! order, flush points and stack size. One rule differs: `real_constant`
//! there reuses a number only among the last 20 constants (`LIM`, as in
//! stock Lua 3.2), but no shipped chunk holds a number twice and they
//! reuse numbers up to 4,226 constants back. The compiler that made them
//! searched the whole table, and so does this one. The accepted language is the
//! one the scene scripts use: statements `name = expression` (optionally
//! ending in `;`), and expressions built from numerals with an optional
//! leading `-`, strings, global names, table constructors, `+` and
//! brackets. Anything else is an error, not a different program.

use std::fmt;

use crate::chunk::{Chunk, Constant, op};
use crate::number;

const MAX_BYTE: usize = 0xFF;
const MAX_WORD: usize = 0xFFFF;
const MAX_ARG: usize = 0xFF_FFFF;
/// `RFIELDS_PER_FLUSH` and `LFIELDS_PER_FLUSH` in `lopcodes.h`.
const RFIELDS_PER_FLUSH: usize = 32;
const LFIELDS_PER_FLUSH: usize = 64;

#[derive(Clone, Debug, PartialEq)]
enum Token {
    Name(String),
    Number(f64),
    String(Vec<u8>),
    Char(u8),
    Eos,
}

const KEYWORDS: [&str; 16] = [
    "and", "do", "else", "elseif", "end", "function", "if", "local", "nil", "not", "or", "repeat",
    "return", "then", "until", "while",
];

struct Lexer<'a> {
    src: &'a [u8],
    at: usize,
    line: usize,
}

impl Lexer<'_> {
    fn error<T>(&self, message: impl std::fmt::Display) -> Result<T, String> {
        Err(format!("line {}: {message}", self.line))
    }

    fn peek(&self) -> Option<u8> {
        self.src.get(self.at).copied()
    }

    fn next(&mut self) -> Result<Token, String> {
        loop {
            let Some(c) = self.peek() else {
                return Ok(Token::Eos);
            };
            match c {
                b'\n' => {
                    self.line += 1;
                    self.at += 1;
                }
                b' ' | b'\t' | b'\r' => self.at += 1,
                b'-' if self.src.get(self.at + 1) == Some(&b'-') => {
                    while self.peek().is_some_and(|c| c != b'\n') {
                        self.at += 1;
                    }
                }
                b'$' => return self.error("pragmas are outside the supported subset"),
                b'"' | b'\'' => return self.string(c),
                b'0'..=b'9' => return self.number(),
                b'.' if self.src.get(self.at + 1).is_some_and(u8::is_ascii_digit) => {
                    return self.number();
                }
                b'_' | b'a'..=b'z' | b'A'..=b'Z' => {
                    let start = self.at;
                    while self
                        .peek()
                        .is_some_and(|c| c == b'_' || c.is_ascii_alphanumeric())
                    {
                        self.at += 1;
                    }
                    let word = String::from_utf8_lossy(&self.src[start..self.at]).into_owned();
                    if KEYWORDS.contains(&word.as_str()) {
                        return self.error(format!("`{word}` is outside the supported subset"));
                    }
                    return Ok(Token::Name(word));
                }
                b'=' if self.src.get(self.at + 1) == Some(&b'=') => {
                    return self.error("`==` is outside the supported subset");
                }
                b'[' if self.src.get(self.at + 1) == Some(&b'[') => {
                    return self.error("long strings are outside the supported subset");
                }
                b'=' | b'{' | b'}' | b'[' | b']' | b',' | b';' | b'+' | b'-' | b'(' | b')' => {
                    self.at += 1;
                    return Ok(Token::Char(c));
                }
                _ => {
                    return self.error(format!(
                        "`{}` is outside the supported subset",
                        char::from(c).escape_default()
                    ));
                }
            }
        }
    }

    /// `read_number` of `llex.c`: digits, `.`, digits, then `E`, one
    /// sign-or-digit and more digits.
    fn number(&mut self) -> Result<Token, String> {
        let start = self.at;
        let digits = |l: &mut Self| {
            while l.peek().is_some_and(|c| c.is_ascii_digit()) {
                l.at += 1;
            }
        };
        digits(self);
        if self.peek() == Some(b'.') {
            self.at += 1;
            if self.peek() == Some(b'.') {
                return self.error("ambiguous syntax (decimal point x string concatenation)");
            }
        }
        digits(self);
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.at += 2;
            digits(self);
        }
        let text = self.src.get(start..self.at).unwrap_or(&[]);
        let text = String::from_utf8_lossy(text);
        match number::parse(&text) {
            Some(x) => Ok(Token::Number(x)),
            None => self.error("invalid numeric format"),
        }
    }

    /// The string escapes of `llex.c`.
    fn string(&mut self, delimiter: u8) -> Result<Token, String> {
        self.at += 1;
        let mut out = Vec::new();
        loop {
            let Some(c) = self.peek() else {
                return self.error("unfinished string");
            };
            self.at += 1;
            match c {
                b'\n' => return self.error("unfinished string"),
                c if c == delimiter => return Ok(Token::String(out)),
                b'\\' => {
                    let Some(e) = self.peek() else {
                        return self.error("unfinished string");
                    };
                    self.at += 1;
                    match e {
                        b'a' => out.push(0x07),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0C),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'v' => out.push(0x0B),
                        b'\n' => {
                            self.line += 1;
                            out.push(b'\n');
                        }
                        b'0'..=b'9' => {
                            let mut value = u32::from(e - b'0');
                            for _ in 0..2 {
                                match self.peek() {
                                    Some(d @ b'0'..=b'9') => {
                                        value = 10 * value + u32::from(d - b'0');
                                        self.at += 1;
                                    }
                                    _ => break,
                                }
                            }
                            match u8::try_from(value) {
                                Ok(b) => out.push(b),
                                Err(_) => return self.error("escape sequence too large"),
                            }
                        }
                        other => out.push(other),
                    }
                }
                c => out.push(c),
            }
        }
    }
}

/// `vardesc` of `lparser.c`, reduced to the two kinds the subset has.
#[derive(Clone, Copy)]
enum Var {
    /// A global; the index of its name in the constants.
    Global(usize),
    /// A value already on the stack.
    Exp,
}

struct Parser<'a> {
    lex: Lexer<'a>,
    token: Token,
    code: Vec<u8>,
    constants: Vec<Constant>,
    /// The value of each number constant, for reuse.
    values: Vec<Option<f64>>,
    stack: usize,
    max_stack: usize,
}

fn codesize(arg: usize) -> usize {
    if arg <= MAX_BYTE {
        2
    } else if arg <= MAX_WORD {
        3
    } else {
        5
    }
}

impl Parser<'_> {
    fn error<T>(&self, message: impl std::fmt::Display) -> Result<T, String> {
        self.lex.error(message)
    }

    fn advance(&mut self) -> Result<(), String> {
        self.token = self.lex.next()?;
        Ok(())
    }

    fn check(&mut self, c: u8) -> Result<(), String> {
        if self.token != Token::Char(c) {
            return self.error(format!("`{}` expected", char::from(c)));
        }
        self.advance()
    }

    fn delta_stack(&mut self, delta: isize) -> Result<(), String> {
        self.stack = self
            .stack
            .checked_add_signed(delta)
            .expect("the stack never goes below zero");
        if self.stack > self.max_stack {
            if self.stack > MAX_BYTE {
                return self.error("function/expression too complex");
            }
            self.max_stack = self.stack;
        }
        Ok(())
    }

    /// The bytes of `op` with `arg` (`code_oparg_at`).
    fn encode(&self, opcode: u8, arg: usize) -> Result<Vec<u8>, String> {
        if arg <= MAX_BYTE {
            return Ok(vec![opcode, arg as u8]);
        }
        if arg > MAX_ARG {
            return self.error("code too long");
        }
        let mut bytes = Vec::with_capacity(5);
        if arg > MAX_WORD {
            bytes.extend([op::LONGARG, (arg >> 16) as u8]);
        }
        bytes.extend([opcode - 1, ((arg & 0xFFFF) >> 8) as u8, (arg & 0xFF) as u8]);
        Ok(bytes)
    }

    fn code_oparg(&mut self, opcode: u8, arg: usize, delta: isize) -> Result<(), String> {
        self.delta_stack(delta)?;
        let bytes = self.encode(opcode, arg)?;
        debug_assert_eq!(bytes.len(), codesize(arg));
        self.code.extend(bytes);
        Ok(())
    }

    fn code_opcode(&mut self, opcode: u8, delta: isize) -> Result<(), String> {
        self.delta_stack(delta)?;
        self.code.push(opcode);
        Ok(())
    }

    fn code_constant(&mut self, c: usize) -> Result<(), String> {
        self.code_oparg(op::PUSHCONSTANT, c, 1)
    }

    fn next_constant(&mut self, constant: Constant, value: Option<f64>) -> Result<usize, String> {
        if self.constants.len() > MAX_ARG {
            return self.error("constant table overflow");
        }
        self.constants.push(constant);
        self.values.push(value);
        Ok(self.constants.len() - 1)
    }

    fn string_constant(&mut self, s: &[u8]) -> Result<usize, String> {
        let found = self
            .constants
            .iter()
            .position(|c| matches!(c, Constant::String(t) if t == s));
        match found {
            Some(c) => Ok(c),
            None => self.next_constant(Constant::String(s.to_vec()), None),
        }
    }

    /// `real_constant`, searching every earlier constant (see the module
    /// documentation). The reference compares with FCOMP and takes C3, so
    /// an unordered comparison also counts as equal.
    fn real_constant(&mut self, x: f64) -> Result<usize, String> {
        let found = self
            .values
            .iter()
            .rposition(|y| y.is_some_and(|y| y == x || y.is_nan() || x.is_nan()));
        match found {
            Some(c) => Ok(c),
            None => self.next_constant(Constant::Number(number::format_g16(x)), Some(x)),
        }
    }

    /// `code_number`: integers up to `MAX_WORD` in magnitude are pushed
    /// directly, everything else through a constant.
    fn code_number(&mut self, x: f64) -> Result<(), String> {
        let magnitude = x.abs();
        if magnitude <= MAX_WORD as f64 && magnitude.fract() == 0.0 {
            let opcode = if x < 0.0 {
                op::PUSHNUMBERNEG
            } else {
                op::PUSHNUMBER
            };
            return self.code_oparg(opcode, magnitude as usize, 1);
        }
        let c = self.real_constant(x)?;
        self.code_constant(c)
    }

    fn push_var(&mut self, v: Var) -> Result<Var, String> {
        if let Var::Global(c) = v {
            self.code_oparg(op::GETGLOBAL, c, 1)?;
        }
        Ok(Var::Exp)
    }

    fn name(&mut self) -> Result<Option<String>, String> {
        if let Token::Name(n) = &self.token {
            let n = n.clone();
            self.advance()?;
            Ok(Some(n))
        } else {
            Ok(None)
        }
    }

    /// `chunk` and `stat`: a list of global assignments.
    fn chunk(&mut self) -> Result<(), String> {
        while self.token != Token::Eos {
            let Some(name) = self.name()? else {
                return self.error("a statement `name = expression` expected");
            };
            let c = self.string_constant(name.as_bytes())?;
            if matches!(self.token, Token::Char(b'.' | b'[' | b'(' | b',')) {
                return self.error("only `name = expression` statements are supported");
            }
            self.check(b'=')?;
            let v = self.exp0()?;
            if self.token == Token::Char(b',') {
                return self.error("multiple values are outside the supported subset");
            }
            self.push_var(v)?;
            self.code_oparg(op::SETGLOBAL, c, -1)?;
            if self.token == Token::Char(b';') {
                self.advance()?;
            }
        }
        self.code_opcode(op::ENDCODE, 0)
    }

    /// `exp1`: an expression whose value is on the stack.
    fn exp1(&mut self) -> Result<(), String> {
        let v = self.exp0()?;
        self.push_var(v)?;
        Ok(())
    }

    /// `exp0` and `exp2` with only `+`, which leaves the operator stack
    /// of `lparser.c` with one pending `+` at most.
    fn exp0(&mut self) -> Result<Var, String> {
        let mut v = self.simpleexp()?;
        let mut pending = false;
        while self.token == Token::Char(b'+') {
            self.push_var(v)?;
            if pending {
                self.code_opcode(op::ADDOP, -1)?;
            }
            pending = true;
            self.advance()?;
            v = self.simpleexp()?;
            v = self.push_var(v)?;
        }
        if pending {
            v = self.push_var(v)?;
            self.code_opcode(op::ADDOP, -1)?;
        }
        Ok(v)
    }

    /// `prefixexp` and `simpleexp`. A `-` is accepted only before a
    /// numeral, where `lparser.c` folds it into the number.
    fn simpleexp(&mut self) -> Result<Var, String> {
        let negate = self.token == Token::Char(b'-');
        if negate {
            self.advance()?;
            if !matches!(self.token, Token::Number(_)) {
                return self.error("`-` is supported only before a numeral");
            }
        }
        match self.token.clone() {
            Token::Number(x) => {
                self.advance()?;
                self.code_number(if negate { -x } else { x })?;
            }
            Token::String(s) => {
                let c = self.string_constant(&s)?;
                self.code_constant(c)?;
                self.advance()?;
            }
            Token::Char(b'{') => self.constructor()?,
            Token::Char(b'(') => {
                self.advance()?;
                let v = self.exp0()?;
                self.check(b')')?;
                return Ok(v);
            }
            Token::Name(n) => {
                self.advance()?;
                if matches!(self.token, Token::Char(b'.' | b'[' | b'(')) {
                    return self.error("indexing and calls are outside the supported subset");
                }
                return Ok(Var::Global(self.string_constant(n.as_bytes())?));
            }
            _ => return self.error("<expression> expected"),
        }
        Ok(Var::Exp)
    }

    /// `constructor`: `{ part [; part] }`, the creation patched in last.
    fn constructor(&mut self) -> Result<(), String> {
        let pc = self.code.len();
        self.code.extend([0, 0]); // SaveWord
        self.delta_stack(1)?;
        self.check(b'{')?;
        let (mut n, kind) = self.part()?;
        if self.token == Token::Char(b';') {
            self.advance()?;
            let (other_n, other_kind) = self.part()?;
            if kind == other_kind {
                return self.error("invalid constructor syntax");
            }
            n += other_n;
        }
        self.check(b'}')?;
        // fix_opcode: open space for a wide argument, then write it.
        let bytes = self.encode(op::CREATEARRAY, n)?;
        self.code.splice(pc..pc + 2, bytes);
        Ok(())
    }

    /// `part`: returns the number of items and its kind (`Some(false)`
    /// for a list, `Some(true)` for a record, `None` when empty).
    fn part(&mut self) -> Result<(usize, Option<bool>), String> {
        match &self.token {
            Token::Char(b';' | b'}') => Ok((0, None)),
            Token::Name(_) => {
                let v = self.exp0()?;
                if self.token == Token::Char(b'=') {
                    let Var::Global(c) = v else {
                        return self.error("unexpected `=`");
                    };
                    self.code_constant(c)?;
                    self.advance()?;
                    self.exp1()?;
                    Ok((self.recfields()?, Some(true)))
                } else {
                    self.push_var(v)?;
                    Ok((self.listfields()?, Some(false)))
                }
            }
            Token::Char(b'[') => {
                self.recfield()?;
                Ok((self.recfields()?, Some(true)))
            }
            _ => {
                self.exp1()?;
                Ok((self.listfields()?, Some(false)))
            }
        }
    }

    fn at_part_end(&self) -> bool {
        matches!(self.token, Token::Char(b';' | b'}'))
    }

    fn flush_record(&mut self, n: usize) -> Result<(), String> {
        if n > 0 {
            self.code_oparg(op::SETMAP, n - 1, -2 * n as isize)?;
        }
        Ok(())
    }

    fn flush_list(&mut self, m: usize, n: usize) -> Result<(), String> {
        if n > 0 {
            self.code_oparg(op::SETLIST, m, -(n as isize))?;
            self.code.push(n as u8);
        }
        Ok(())
    }

    fn recfields(&mut self) -> Result<usize, String> {
        let mut n = 1;
        while self.token == Token::Char(b',') {
            self.advance()?;
            if self.at_part_end() {
                break;
            }
            self.recfield()?;
            n += 1;
            if n % RFIELDS_PER_FLUSH == 0 {
                self.flush_record(RFIELDS_PER_FLUSH)?;
            }
        }
        self.flush_record(n % RFIELDS_PER_FLUSH)?;
        Ok(n)
    }

    fn listfields(&mut self) -> Result<usize, String> {
        let mut n = 1;
        while self.token == Token::Char(b',') {
            self.advance()?;
            if self.at_part_end() {
                break;
            }
            self.exp1()?;
            n += 1;
            if n % LFIELDS_PER_FLUSH == 0 {
                self.flush_list(n / LFIELDS_PER_FLUSH - 1, LFIELDS_PER_FLUSH)?;
            }
        }
        self.flush_list(n / LFIELDS_PER_FLUSH, n % LFIELDS_PER_FLUSH)?;
        Ok(n)
    }

    fn recfield(&mut self) -> Result<(), String> {
        match self.token.clone() {
            Token::Name(n) => {
                let c = self.string_constant(n.as_bytes())?;
                self.code_constant(c)?;
                self.advance()?;
            }
            Token::Char(b'[') => {
                self.advance()?;
                self.exp1()?;
                self.check(b']')?;
            }
            _ => return self.error("NAME or `[` expected"),
        }
        self.check(b'=')?;
        self.exp1()
    }
}

/// Why source did not compile: the line and what was wrong there.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompileError(pub String);

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for CompileError {}

/// Compiles source in the supported subset to the chunk the reference
/// compiler makes from it. The function header is the one every shipped
/// chunk has: line 0 and an empty source name.
pub fn compile(src: &[u8]) -> Result<Chunk, CompileError> {
    compile_chunk(src).map_err(CompileError)
}

fn compile_chunk(src: &[u8]) -> Result<Chunk, String> {
    let lex = Lexer {
        src,
        at: 0,
        line: 1,
    };
    let mut p = Parser {
        lex,
        token: Token::Eos,
        code: vec![0, 0], // stack size and parameter count, patched below
        constants: Vec::new(),
        values: Vec::new(),
        stack: 0,
        max_stack: 0,
    };
    p.advance()?;
    p.chunk()?;
    p.code[0] = p.max_stack as u8;
    Ok(Chunk {
        line_defined: 0,
        source: Some(Vec::new()),
        code: p.code,
        constants: p.constants,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::chunk::HEADER;

    /// Code worked out by hand from `lparser.c` for a small script.
    #[test]
    fn compiles_as_the_reference_parser_does() {
        let chunk = compile(b"x = 1\ny = { -2, 0.5; a = Z + 3 }\n").unwrap();
        assert_eq!(
            chunk.constants,
            [
                Constant::String(b"x".to_vec()),
                Constant::String(b"y".to_vec()),
                Constant::Number("0.5".into()),
                Constant::String(b"a".to_vec()),
                Constant::String(b"Z".to_vec()),
            ]
        );
        #[rustfmt::skip]
        let code = [
            4, 0,                           // stack size 4, no parameters
            op::PUSHNUMBER, 1,
            op::SETGLOBAL, 0,               // x = 1
            op::CREATEARRAY, 3,             // { ... } with 3 items in all
            op::PUSHNUMBERNEG, 2,
            op::PUSHCONSTANT, 2,
            op::SETLIST, 0, 2,              // t[1], t[2]
            op::PUSHCONSTANT, 3,            // "a"
            op::GETGLOBAL, 4,
            op::PUSHNUMBER, 3,
            op::ADDOP,
            op::SETMAP, 0,                  // t.a
            op::SETGLOBAL, 1,               // y = t
            op::ENDCODE,
        ];
        assert_eq!(chunk.code, code);
        let mut header = HEADER.to_vec();
        header.extend([0, 0, 0, 0, 0, 0, 0, 0, 1, 0]);
        assert!(chunk.write().starts_with(&header));
        assert_eq!(Chunk::read(&chunk.write()), Ok(chunk));
    }

    #[test]
    fn rejects_what_the_subset_does_not_cover() {
        for src in [
            "x = f(1)",
            "x = a.b",
            "local x = 1",
            "x = -y",
            "x = 1 - 2",
            "x, y = 1, 2",
            "x = [[long]]",
            "$debug\nx = 1",
            "x = 1 == 2",
        ] {
            assert!(compile(src.as_bytes()).is_err(), "{src}");
        }
    }
}
