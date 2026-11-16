//! The memory backend (`FMemFileSystem`): one growable or fixed buffer.
//! No game code creates one; it is kept for completeness and because the
//! UTF backend can read an archive from it.

use super::path::{ROOT_PATH_CHARS, copy_path, mem_resolve_path};
use super::{FileSystem, FindEntry, FsDesc, FsKind, GENERIC_WRITE, SeekOrigin, VfsError, VfsFile};

/// Where the buffer comes from (the descriptor's buffer, size and flag bit
/// 0).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MemSource<'a> {
    /// Flag bit 0 clear: an own buffer the size of the data, holding a copy
    /// of it, that grows on writes. Without data the buffer is empty.
    Copy(Option<&'a [u8]>),
    /// Flag bit 0 set: the given buffer, whose size is fixed.
    Wrap(Vec<u8>),
}

/// A buffer used as one file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MemFileSystem {
    /// The allocation; its length is the C++'s allocated size.
    buffer: Vec<u8>,
    fixed: bool,
    access: u32,
    pos: u32,
    logical: u32,
    root_path: Vec<u8>,
}

/// The C++'s growth rule: the end plus 0xFFFF, rounded down to 4 KiB, with
/// 32-bit wrapping.
fn grown_size(end: u32) -> u32 {
    end.wrapping_add(0xFFFF) & 0xFFFF_F000
}

impl MemFileSystem {
    /// `InitializeFromDescriptor`. The logical size starts at the buffer
    /// size when `creation` is 3 or 4 (`OPEN_EXISTING`,
    /// `TRUNCATE_EXISTING`), else at 0. `path` becomes the root path, cut
    /// to 263 bytes.
    #[must_use]
    pub fn new(path: Option<&[u8]>, access: u32, creation: u32, source: MemSource<'_>) -> Self {
        let (buffer, fixed) = match source {
            MemSource::Copy(data) => (data.map(<[u8]>::to_vec).unwrap_or_default(), false),
            MemSource::Wrap(buffer) => (buffer, true),
        };
        let alloc = Self::len_of(&buffer);
        Self {
            buffer,
            fixed,
            access,
            pos: 0,
            logical: if (3..5).contains(&creation) { alloc } else { 0 },
            root_path: path
                .map(|p| copy_path(p, ROOT_PATH_CHARS))
                .unwrap_or_default(),
        }
    }

    fn len_of(buffer: &[u8]) -> u32 {
        u32::try_from(buffer.len()).unwrap_or(u32::MAX)
    }

    fn alloc(&self) -> u32 {
        Self::len_of(&self.buffer)
    }

    /// `IDataBuffer::GetBuffer`: the whole allocation, which is larger than
    /// the logical size after growth.
    #[must_use]
    pub fn buffer(&self) -> &[u8] {
        &self.buffer
    }

    /// The logical size, what [`FileSystem::size`] returns.
    #[must_use]
    pub fn logical_size(&self) -> u32 {
        self.logical
    }

    /// Grows the allocation to `new_size`, zero-filled; `false` where
    /// `VirtualAlloc` would fail.
    fn grow(&mut self, new_size: u32) -> bool {
        // A size that wrapped below the allocation makes the reference copy
        // past its new buffer; here it fails as an allocation would.
        if new_size == 0 || (new_size as usize) < self.buffer.len() {
            return false;
        }
        let extra = (new_size as usize).saturating_sub(self.buffer.len());
        if self.buffer.try_reserve_exact(extra).is_err() {
            return false;
        }
        self.buffer.resize(new_size as usize, 0);
        true
    }

    /// `Write(0, data, ..., position)`: `position` stands for the
    /// overlapped offset. A fixed buffer takes what fits; an own buffer
    /// grows. Returns the bytes written.
    ///
    /// # Errors
    ///
    /// [`VfsError::Failed`] without write access.
    pub fn write_at(&mut self, data: &[u8], position: Option<u32>) -> Result<usize, VfsError> {
        if self.access & GENERIC_WRITE == 0 {
            return Err(VfsError::Failed);
        }
        if let Some(position) = position {
            self.pos = position;
        }
        let size = Self::len_of(data);
        let mut to_write = size;
        let end = self.pos.wrapping_add(size);
        let alloc = self.alloc();
        if end > alloc {
            if self.fixed {
                to_write = alloc.saturating_sub(self.pos);
            } else if !self.grow(grown_size(end)) {
                to_write = 0;
            }
        }
        let start = self.pos as usize;
        // A wrapped end makes the reference write outside its buffer.
        if start + to_write as usize > self.buffer.len() {
            to_write = 0;
        }
        if to_write != 0 {
            self.buffer[start..start + to_write as usize]
                .copy_from_slice(&data[..to_write as usize]);
        }
        self.pos = self.pos.wrapping_add(to_write);
        if to_write != 0 && self.logical < self.pos {
            self.logical = self.pos;
        }
        Ok(to_write as usize)
    }

    /// `Read(0, buf, ..., position)`: from the position, clamped to the
    /// logical size. Always succeeds.
    ///
    /// # Errors
    ///
    /// None; the result type matches the trait.
    pub fn read_at(&mut self, buf: &mut [u8], position: Option<u32>) -> Result<usize, VfsError> {
        if let Some(position) = position {
            self.pos = position;
        }
        self.pos = self.pos.min(self.logical);
        let remain = self.logical - self.pos;
        // A logical size past the allocation makes the reference read
        // outside it.
        let available = self.alloc().saturating_sub(self.pos);
        let got = (Self::len_of(buf).min(remain).min(available)) as usize;
        let start = self.pos as usize;
        buf[..got].copy_from_slice(&self.buffer[start..start + got]);
        self.pos += got as u32;
        Ok(got)
    }
}

impl FileSystem for MemFileSystem {
    fn kind(&self) -> FsKind {
        FsKind::Memory
    }

    fn create_child(&self, _desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
        Err(VfsError::Generic)
    }

    fn open_file(&self, _desc: &FsDesc<'_>) -> Result<VfsFile, VfsError> {
        Err(VfsError::Failed)
    }

    fn open_node(&mut self, _name: &[u8]) -> Result<(), VfsError> {
        Err(VfsError::Failed)
    }

    /// The C++ returns 0 and no error.
    fn path_attributes(&self, _path: &[u8]) -> Result<u32, VfsError> {
        Ok(0)
    }

    fn find(&self, _pattern: &[u8]) -> Result<Vec<FindEntry>, VfsError> {
        Err(VfsError::Failed)
    }

    fn resolve_path(&self, src: &[u8], dest_chars: usize) -> (bool, Vec<u8>) {
        mem_resolve_path(&self.root_path, src, dest_chars)
    }

    fn file_sys_path(&self, max_chars: usize) -> Vec<u8> {
        let count = (self.root_path.len() + 1).min(max_chars);
        self.root_path[..count.saturating_sub(1)].to_vec()
    }

    fn current_path(&self, _max_chars: usize) -> Result<Vec<u8>, VfsError> {
        Err(VfsError::Failed)
    }

    fn access(&self) -> u32 {
        self.access
    }

    fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        self.read_at(buf, None)
    }

    fn size(&self) -> Result<u32, VfsError> {
        Ok(self.logical)
    }

    /// The end origin counts from the allocation, not the logical size;
    /// nothing is clamped.
    fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        self.pos = match origin {
            SeekOrigin::Begin => offset,
            SeekOrigin::Current => self.pos.wrapping_add(offset),
            SeekOrigin::End => self.alloc().wrapping_sub(offset),
        };
        Ok(self.pos)
    }

    fn position(&self) -> Result<u32, VfsError> {
        Ok(self.pos)
    }

    fn map_view(&self, _offset: u32, _byte_count: u32) -> Result<&[u8], VfsError> {
        Err(VfsError::Failed)
    }

    fn write(&mut self, data: &[u8]) -> Result<usize, VfsError> {
        self.write_at(data, None)
    }

    /// The logical size becomes the position; an own buffer grows to hold
    /// it (the size is set even when growing fails), a fixed one refuses.
    fn set_end_of_file(&mut self) -> Result<(), VfsError> {
        if self.access & GENERIC_WRITE == 0 {
            return Err(VfsError::Failed);
        }
        if self.pos > self.alloc() {
            if self.fixed {
                return Err(VfsError::Failed);
            }
            self.grow(grown_size(self.pos));
        }
        self.logical = self.pos;
        Ok(())
    }
}
