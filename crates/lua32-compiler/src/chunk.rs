//! The precompiled Lua 3.2 chunk format, as `lundump.c` in
//! `CppSource/SceneScript/lua32/src` reads it.
//!
//! A chunk is `ESC "Lua"`, the version byte `0x32`, the size of a number
//! (0: numbers are stored as text), then the main function. All integers
//! are big-endian: a word is 2 bytes, a long 4. The function is its line,
//! its source name, its code, its local names and its constants. A string
//! is a long holding its length plus one, then its bytes and a NUL; a long
//! of 0 is no string. A number constant is a length byte and its text.

use std::fmt;

/// The signature and the version byte every shipped chunk starts with.
pub const HEADER: &[u8; 5] = b"\x1bLua\x32";

/// Opcodes of the Lua 3.2 virtual machine (`lopcodes.h`), in their order.
/// The word-argument variant of an opcode is the one before it.
pub mod op {
    pub const ENDCODE: u8 = 0;
    pub const PUSHNUMBERW: u8 = 6;
    pub const PUSHNUMBER: u8 = 7;
    pub const PUSHNUMBERNEGW: u8 = 8;
    pub const PUSHNUMBERNEG: u8 = 9;
    pub const PUSHCONSTANTW: u8 = 10;
    pub const PUSHCONSTANT: u8 = 11;
    pub const GETGLOBALW: u8 = 14;
    pub const GETGLOBAL: u8 = 15;
    pub const CREATEARRAYW: u8 = 21;
    pub const CREATEARRAY: u8 = 22;
    pub const SETGLOBALW: u8 = 24;
    pub const SETGLOBAL: u8 = 25;
    pub const SETLISTW: u8 = 28;
    pub const SETLIST: u8 = 29;
    pub const SETMAP: u8 = 30;
    pub const ADDOP: u8 = 37;
    pub const LONGARGW: u8 = 61;
    pub const LONGARG: u8 = 62;
}

/// Constant type bytes: the negated `LUA_T_*` tags of `lobject.h`.
const T_NUMBER: u8 = 1;
const T_STRING: u8 = 2;
const T_PROTO: u8 = 4;
const T_NIL: u8 = 6;

/// One entry of a function's constant table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Constant {
    /// A number in its stored text, as the compiler printed it.
    Number(String),
    /// A string's bytes, without the terminating NUL.
    String(Vec<u8>),
}

/// A chunk whose main function has no nested functions and no local
/// names, which is the form of every shipped `.thn` script.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Chunk {
    pub line_defined: u32,
    /// The source name; `None` is the empty record (length 0).
    pub source: Option<Vec<u8>>,
    /// The code: the stack size byte, the parameter byte, then the
    /// instructions up to and including `ENDCODE`.
    pub code: Vec<u8>,
    pub constants: Vec<Constant>,
}

#[derive(Debug, PartialEq, Eq)]
pub struct ChunkError(pub String);

impl fmt::Display for ChunkError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ChunkError {}

fn error<T>(message: impl Into<String>) -> Result<T, ChunkError> {
    Err(ChunkError(message.into()))
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], ChunkError> {
        let Some(slice) = self.bytes.get(self.at..self.at + n) else {
            return error(format!("unexpected end of chunk at byte {}", self.at));
        };
        self.at += n;
        Ok(slice)
    }

    fn byte(&mut self) -> Result<u8, ChunkError> {
        Ok(self.take(1)?[0])
    }

    fn long(&mut self) -> Result<u32, ChunkError> {
        let b = self.take(4)?;
        Ok(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn count(&mut self) -> Result<usize, ChunkError> {
        let n = self.long()?;
        usize::try_from(n).map_err(|_| ChunkError(format!("count {n} too large")))
    }

    fn string(&mut self) -> Result<Option<Vec<u8>>, ChunkError> {
        let size = self.count()?;
        if size == 0 {
            return Ok(None);
        }
        let bytes = self.take(size)?;
        // lundump keeps size-1 bytes; the last is the terminator.
        Ok(Some(bytes[..size - 1].to_vec()))
    }
}

impl Chunk {
    /// Reads a chunk. Native-number chunks, nested functions, local names
    /// and nil constants are rejected: no shipped script has them.
    pub fn read(bytes: &[u8]) -> Result<Chunk, ChunkError> {
        let mut r = Reader { bytes, at: 0 };
        if r.take(HEADER.len()).ok() != Some(&HEADER[..]) {
            return error("not a precompiled Lua 3.2 chunk");
        }
        if r.byte()? != 0 {
            return error("the chunk stores numbers in native form, which is not supported");
        }
        let line_defined = r.long()?;
        let source = r.string()?;
        let code_size = r.count()?;
        let code = r.take(code_size)?.to_vec();
        if code.len() < 3 || code.last() != Some(&op::ENDCODE) {
            return error("bad code block");
        }
        if r.long()? != 0 {
            return error("the chunk has local variable names, which is not supported");
        }
        let n = r.count()?;
        let mut constants = Vec::with_capacity(n.min(bytes.len()));
        for i in 0..n {
            constants.push(match r.byte()? {
                T_NUMBER => {
                    let len = usize::from(r.byte()?);
                    let text = r.take(len)?;
                    let text = std::str::from_utf8(text)
                        .ok()
                        .filter(|t| !t.is_empty())
                        .ok_or_else(|| ChunkError(format!("constant {i}: bad number text")))?;
                    Constant::Number(text.to_owned())
                }
                T_STRING => Constant::String(
                    r.string()?
                        .ok_or_else(|| ChunkError(format!("constant {i}: missing string")))?,
                ),
                T_PROTO => return error("the chunk has nested functions, which is not supported"),
                T_NIL => return error("the chunk has a nil constant, which is not supported"),
                other => return error(format!("constant {i}: unknown type {other}")),
            });
        }
        if r.at != bytes.len() {
            return error(format!("{} bytes after the chunk", bytes.len() - r.at));
        }
        Ok(Chunk {
            line_defined,
            source,
            code,
            constants,
        })
    }

    /// Writes the chunk in the form [`Chunk::read`] accepts.
    pub fn write(&self) -> Vec<u8> {
        fn long(out: &mut Vec<u8>, n: usize) {
            let n = u32::try_from(n).expect("chunk sizes fit in 32 bits");
            out.extend_from_slice(&n.to_be_bytes());
        }
        fn string(out: &mut Vec<u8>, s: Option<&[u8]>) {
            match s {
                None => long(out, 0),
                Some(s) => {
                    long(out, s.len() + 1);
                    out.extend_from_slice(s);
                    out.push(0);
                }
            }
        }
        let mut out = Vec::new();
        out.extend_from_slice(HEADER);
        out.push(0);
        out.extend_from_slice(&self.line_defined.to_be_bytes());
        string(&mut out, self.source.as_deref());
        long(&mut out, self.code.len());
        out.extend_from_slice(&self.code);
        long(&mut out, 0);
        long(&mut out, self.constants.len());
        for constant in &self.constants {
            match constant {
                Constant::Number(text) => {
                    out.push(T_NUMBER);
                    out.push(u8::try_from(text.len()).expect("number text fits in a byte"));
                    out.extend_from_slice(text.as_bytes());
                }
                Constant::String(s) => {
                    out.push(T_STRING);
                    string(&mut out, Some(s));
                }
            }
        }
        out
    }
}
