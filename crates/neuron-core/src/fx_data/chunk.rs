//! The chunk stream the loaders read (`FAleChunk` in
//! `particlefx_types.cpp`): one file inside a UTF directory, read in
//! order. Every read either fills its buffer or fails; a failed read has
//! still moved the position as far as the file system read.

use crate::vfs::{FileSystem, FsDesc, VfsFile};

/// A read that did not fill its buffer (`ReadBytes` returning -1).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Short;

/// An open chunk. A chunk whose file did not open fails every read, as the
/// reference's reads through handle -1 do.
#[derive(Debug)]
pub(crate) struct Chunk {
    file: Option<VfsFile>,
    /// A read copied nothing: the position is at the end (or the file did
    /// not open), so every later read fails too.
    exhausted: bool,
}

impl Chunk {
    /// `OpenChunk(fs, name, 0)`: the leaf `name` in the open node.
    pub(crate) fn open(fs: &dyn FileSystem, name: &[u8]) -> Self {
        let file = fs.open_file(&FsDesc::read(name)).ok();
        Self {
            exhausted: file.is_none(),
            file,
        }
    }

    /// Whether a read has copied nothing: the position is at the end of
    /// the node, or past the end of a truncated archive, and every later
    /// read copies nothing too. (Only a 32-bit wrap of the file offset,
    /// which has the reference read outside the mapped archive, could make
    /// one copy again.) The effect loader uses this to end a loop whose
    /// remaining iterations the reference runs as failed reads, up to 2^31
    /// of them.
    pub(crate) const fn exhausted(&self) -> bool {
        self.exhausted
    }

    /// `ReadBytes`: succeeds only when the whole buffer was read.
    pub(crate) fn read(&mut self, buf: &mut [u8]) -> Result<(), Short> {
        let file = self.file.as_mut().ok_or(Short)?;
        match file.read(buf) {
            Ok(count) if count == buf.len() => Ok(()),
            Ok(count) => {
                self.exhausted |= count == 0 && !buf.is_empty();
                Err(Short)
            }
            Err(_) => {
                self.exhausted |= !buf.is_empty();
                Err(Short)
            }
        }
    }

    pub(crate) fn bytes<const N: usize>(&mut self) -> Result<[u8; N], Short> {
        let mut buf = [0; N];
        self.read(&mut buf)?;
        Ok(buf)
    }

    pub(crate) fn u8(&mut self) -> Result<u8, Short> {
        Ok(self.bytes::<1>()?[0])
    }

    pub(crate) fn u16(&mut self) -> Result<u16, Short> {
        Ok(u16::from_le_bytes(self.bytes()?))
    }

    pub(crate) fn u32(&mut self) -> Result<u32, Short> {
        Ok(u32::from_le_bytes(self.bytes()?))
    }

    pub(crate) fn i32(&mut self) -> Result<i32, Short> {
        Ok(i32::from_le_bytes(self.bytes()?))
    }

    pub(crate) fn f32(&mut self) -> Result<f32, Short> {
        Ok(f32::from_bits(self.u32()?))
    }

    /// `ReadString(buffer, max)`: a 16-bit length, that many bytes and one
    /// pad byte after an odd length. A length of 0 is the empty string and
    /// a length above `max` fails after the length was read. The result of
    /// an odd-length string is the pad read's.
    ///
    /// The text is the bytes up to the first NUL. The reference copies the
    /// bytes into a 256-byte stack buffer and reads it as a C string, so
    /// bytes without a NUL run on into whatever the buffer held before;
    /// here the text ends with the bytes read (divergence register).
    pub(crate) fn string(&mut self, max: usize) -> Result<Vec<u8>, Short> {
        let length = usize::from(self.u16()?);
        if length == 0 {
            return Ok(Vec::new());
        }
        if length > max {
            return Err(Short);
        }
        let mut text = vec![0; length];
        let mut result = self.read(&mut text);
        if length % 2 == 1 {
            result = self.read(&mut [0]);
        }
        result?;
        if let Some(end) = text.iter().position(|&b| b == 0) {
            text.truncate(end);
        }
        Ok(text)
    }
}
