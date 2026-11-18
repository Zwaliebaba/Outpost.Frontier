//! The precompiled Lua 3.2 chunk, as `lundump.c` in
//! `CppSource/SceneScript/lua32/src` reads it, restricted to the form of
//! the shipped scene scripts (design/facts.md F15, decision O18).
//!
//! A chunk is `ESC "Lua"`, the version byte `0x32`, the size of a number
//! (0: numbers are stored as text), then the main function: its line, its
//! source name, its code, its local names and its constants. Integers are
//! big-endian; a long is 4 bytes. A string is a long holding its length
//! plus one, then its bytes and a NUL; a long of 0 is no string. A number
//! constant is a length byte and its text.

use std::fmt;

use super::number;

/// The signature and the version byte every accepted chunk starts with.
pub const HEADER: &[u8; 5] = b"\x1bLua\x32";

/// `ID_CHUNK`: the first byte of a compiled chunk.
pub const ID_CHUNK: u8 = 0x1b;

/// Opcodes of the Lua 3.2 virtual machine (`lopcodes.h`) in the subset the
/// scene scripts use. The word-argument variant of an opcode is the one
/// before it.
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
    pub const LONGARGW: u8 = 64;
    pub const LONGARG: u8 = 65;
}

/// Constant type bytes: the negated `LUA_T_*` tags of `lobject.h`.
const T_NUMBER: u8 = 1;
const T_STRING: u8 = 2;
const T_PROTO: u8 = 4;
const T_NIL: u8 = 6;

/// One entry of a function's constant table.
#[derive(Clone, Debug, PartialEq)]
pub enum Constant {
    /// A number, read from its stored text by `luaU_str2d`.
    Number(f64),
    /// A string's bytes, without the terminating NUL.
    String(Vec<u8>),
}

/// The main function of a chunk with no nested functions and no local
/// names.
#[derive(Clone, Debug, PartialEq)]
pub struct Chunk {
    /// The code: the stack size byte, the parameter byte, then the
    /// instructions up to and including the final `ENDCODE`.
    pub code: Vec<u8>,
    pub constants: Vec<Constant>,
}

/// Why a chunk was not read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ChunkError {
    /// The bytes do not start with `ESC "Lua"` and version 0x32.
    NotACompiledChunk,
    /// The chunk stores numbers in native binary form.
    NativeNumbers,
    Truncated {
        at: usize,
    },
    /// The code block is shorter than three bytes or does not end with
    /// `ENDCODE` ("bad code").
    BadCode,
    /// The main function takes parameters.
    Parameters,
    LocalNames,
    NestedFunction {
        constant: usize,
    },
    NilConstant {
        constant: usize,
    },
    /// A string constant with no string record.
    MissingString {
        constant: usize,
    },
    /// A number constant the reference cannot convert.
    BadNumber {
        constant: usize,
    },
    UnknownConstantType {
        constant: usize,
        tag: u8,
    },
}

impl fmt::Display for ChunkError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotACompiledChunk => f.write_str("not a precompiled Lua 3.2 chunk"),
            Self::NativeNumbers => f.write_str("the chunk stores numbers in native form"),
            Self::Truncated { at } => write!(f, "unexpected end of chunk at byte {at}"),
            Self::BadCode => f.write_str("bad code block"),
            Self::Parameters => f.write_str("the main function takes parameters"),
            Self::LocalNames => f.write_str("the chunk has local variable names"),
            Self::NestedFunction { constant } => {
                write!(f, "constant {constant} is a nested function")
            }
            Self::NilConstant { constant } => write!(f, "constant {constant} is nil"),
            Self::MissingString { constant } => {
                write!(f, "constant {constant} has no string")
            }
            Self::BadNumber { constant } => {
                write!(f, "constant {constant} is not a number the reference reads")
            }
            Self::UnknownConstantType { constant, tag } => {
                write!(f, "constant {constant} has unknown type {tag}")
            }
        }
    }
}

impl std::error::Error for ChunkError {}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], ChunkError> {
        let slice = self
            .at
            .checked_add(n)
            .and_then(|end| self.bytes.get(self.at..end))
            .ok_or(ChunkError::Truncated { at: self.at })?;
        self.at += n;
        Ok(slice)
    }

    fn byte(&mut self) -> Result<u8, ChunkError> {
        Ok(self.take(1)?[0])
    }

    fn long(&mut self) -> Result<usize, ChunkError> {
        let b = self.take(4)?;
        let n = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
        usize::try_from(n).map_err(|_| ChunkError::Truncated { at: self.at })
    }

    fn string(&mut self) -> Result<Option<Vec<u8>>, ChunkError> {
        let size = self.long()?;
        if size == 0 {
            return Ok(None);
        }
        let bytes = self.take(size)?;
        // lundump keeps size - 1 bytes; the last is the terminator.
        Ok(Some(bytes[..size - 1].to_vec()))
    }
}

impl Chunk {
    /// Reads one chunk from the start of `bytes` and returns it with the
    /// number of bytes it took. `lua_dobuffer` runs the chunks of a buffer
    /// one after the other.
    pub fn read(bytes: &[u8]) -> Result<(Chunk, usize), ChunkError> {
        let mut r = Reader { bytes, at: 0 };
        if bytes.get(..HEADER.len()) != Some(&HEADER[..]) {
            return Err(ChunkError::NotACompiledChunk);
        }
        r.at = HEADER.len();
        if r.byte()? != 0 {
            return Err(ChunkError::NativeNumbers);
        }
        let _line_defined = r.long()?;
        let _source = r.string()?;
        let code_size = r.long()?;
        let code = r.take(code_size)?.to_vec();
        if code.len() < 3 || code.last() != Some(&op::ENDCODE) {
            return Err(ChunkError::BadCode);
        }
        if code[1] != 0 {
            return Err(ChunkError::Parameters);
        }
        if r.long()? != 0 {
            return Err(ChunkError::LocalNames);
        }
        let n = r.long()?;
        let mut constants = Vec::with_capacity(n.min(bytes.len()));
        for constant in 0..n {
            constants.push(match r.byte()? {
                T_NUMBER => {
                    let len = usize::from(r.byte()?);
                    let text = r.take(len)?;
                    Constant::Number(
                        number::undump(text).ok_or(ChunkError::BadNumber { constant })?,
                    )
                }
                T_STRING => {
                    Constant::String(r.string()?.ok_or(ChunkError::MissingString { constant })?)
                }
                T_PROTO => return Err(ChunkError::NestedFunction { constant }),
                T_NIL => return Err(ChunkError::NilConstant { constant }),
                tag => return Err(ChunkError::UnknownConstantType { constant, tag }),
            });
        }
        Ok((Chunk { code, constants }, r.at))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chunk(code: &[u8], constants: &[u8], count: u32) -> Vec<u8> {
        let mut out = HEADER.to_vec();
        out.push(0);
        out.extend_from_slice(&0u32.to_be_bytes());
        out.extend_from_slice(&0u32.to_be_bytes());
        out.extend_from_slice(&u32::try_from(code.len()).unwrap().to_be_bytes());
        out.extend_from_slice(code);
        out.extend_from_slice(&0u32.to_be_bytes());
        out.extend_from_slice(&count.to_be_bytes());
        out.extend_from_slice(constants);
        out
    }

    #[test]
    fn reads_constants() {
        let mut constants = vec![T_NUMBER, 4];
        constants.extend_from_slice(b"-1.5");
        constants.push(T_STRING);
        constants.extend_from_slice(&3u32.to_be_bytes());
        constants.extend_from_slice(b"ab\0");
        let bytes = chunk(&[2, 0, op::ENDCODE], &constants, 2);
        let (c, used) = Chunk::read(&bytes).unwrap();
        assert_eq!(used, bytes.len());
        assert_eq!(
            c.constants,
            vec![Constant::Number(-1.5), Constant::String(b"ab".to_vec())]
        );
    }

    #[test]
    fn rejects_what_the_scripts_never_use() {
        assert_eq!(
            Chunk::read(b"-- source").unwrap_err(),
            ChunkError::NotACompiledChunk
        );
        let mut native = chunk(&[2, 0, 0], &[], 0);
        native[5] = 8;
        assert_eq!(Chunk::read(&native).unwrap_err(), ChunkError::NativeNumbers);
        assert_eq!(
            Chunk::read(&chunk(&[2, 1, 0], &[], 0)).unwrap_err(),
            ChunkError::Parameters
        );
        assert_eq!(
            Chunk::read(&chunk(&[2, 0, 7], &[], 0)).unwrap_err(),
            ChunkError::BadCode
        );
        assert_eq!(
            Chunk::read(&chunk(&[2, 0, 0], &[T_NIL], 1)).unwrap_err(),
            ChunkError::NilConstant { constant: 0 }
        );
        assert_eq!(
            Chunk::read(&chunk(&[2, 0, 0], &[T_NUMBER, 1, b'x'], 1)).unwrap_err(),
            ChunkError::BadNumber { constant: 0 }
        );
        let bytes = chunk(&[2, 0, 0], &[], 0);
        assert_eq!(
            Chunk::read(&bytes[..bytes.len() - 2]).unwrap_err(),
            ChunkError::Truncated {
                at: bytes.len() - 4
            }
        );
    }
}
