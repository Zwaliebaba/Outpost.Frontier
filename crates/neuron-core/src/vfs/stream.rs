//! Opened files: the DOS and UTF read rules, which differ.

use std::sync::Arc;

use super::utf_fs::UtfShared;
use super::{GENERIC_WRITE, SeekOrigin, VfsError};
use crate::utf::{Layout, NodeRef, packed_to_file_time};

/// A host file read whole (`FDosOpenFile`).
#[derive(Clone, Debug)]
pub(crate) struct DosStream {
    data: Arc<[u8]>,
    pos: u32,
    times: [u64; 3],
    access: u32,
}

impl DosStream {
    pub(crate) fn new(data: Vec<u8>, times: [u64; 3], access: u32) -> Self {
        Self {
            data: data.into(),
            pos: 0,
            times,
            access,
        }
    }

    fn len(&self) -> u32 {
        u32::try_from(self.data.len()).unwrap_or(u32::MAX)
    }

    /// Windows cannot map an empty file, and every operation that needs
    /// the view fails on one.
    fn mapped(&self) -> bool {
        !self.data.is_empty()
    }

    /// `Read`: copies what is left up to `buf.len()` bytes, and fails
    /// unless that is all of `buf`.
    pub(crate) fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        let mut got = 0;
        if self.mapped() && self.pos < self.len() {
            got = buf.len().min((self.len() - self.pos) as usize);
            let start = self.pos as usize;
            buf[..got].copy_from_slice(&self.data[start..start + got]);
            self.pos += got as u32;
        }
        if got == buf.len() {
            Ok(got)
        } else {
            Err(VfsError::ShortRead { read: got })
        }
    }

    pub(crate) fn size(&self) -> u32 {
        self.len()
    }

    /// `SeekFile` without a high-dword pointer: the end origin subtracts the
    /// offset from the size; position 0xFFFFFFFF is stored but reported as
    /// a failure.
    pub(crate) fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        if !self.mapped() {
            return Err(VfsError::SeekFailed);
        }
        let target = match origin {
            SeekOrigin::Begin => offset,
            SeekOrigin::Current => self.pos.wrapping_add(offset),
            SeekOrigin::End => self.len().wrapping_sub(offset),
        };
        self.pos = target;
        if target == u32::MAX {
            return Err(VfsError::SeekFailed);
        }
        Ok(target)
    }

    pub(crate) fn position(&self) -> Result<u32, VfsError> {
        if self.mapped() {
            Ok(self.pos)
        } else {
            Err(VfsError::SeekFailed)
        }
    }

    /// `CreateMapping` and `MapFileView`.
    pub(crate) fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError> {
        if self.access & GENERIC_WRITE != 0 || !self.mapped() {
            return Err(VfsError::AccessDenied);
        }
        if byte_count > self.len().wrapping_sub(offset) {
            return Err(VfsError::AccessDenied);
        }
        // An offset past the end passes the reference's check and points
        // outside the view.
        if offset > self.len() {
            return Err(VfsError::InvalidParameter);
        }
        let count = if byte_count == 0 {
            self.len() - offset
        } else {
            byte_count
        };
        Ok(&self.data[offset as usize..(offset + count) as usize])
    }

    pub(crate) fn times(&self) -> [Option<u64>; 3] {
        self.times.map(Some)
    }

    pub(crate) fn bytes(&self) -> &[u8] {
        &self.data
    }
}

/// A UTF node opened for reading (`FUtfHandle`).
#[derive(Clone, Debug)]
pub(crate) struct UtfStream {
    pub(crate) shared: Arc<UtfShared>,
    pub(crate) node: NodeRef,
    pub(crate) pos: u32,
    /// `CreateMapping` works only on a file system made from another one.
    pub(crate) mappable: bool,
}

impl UtfStream {
    /// `Read` of the UTF backend: an empty buffer is error 0x57; the read
    /// stops at the node size and at the end of the archive; the position
    /// moves to the end the node size allows even when the archive cuts the
    /// copy; a read that copies nothing is error 0x1E.
    pub(crate) fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        utf_read(&self.shared, self.node, &mut self.pos, buf)
    }

    pub(crate) fn size(&self) -> Result<u32, VfsError> {
        Ok(self.shared.archive.size(self.node)?)
    }

    pub(crate) fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        utf_seek(&self.shared, self.node, &mut self.pos, offset, origin)
    }

    pub(crate) fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError> {
        utf_map_view(&self.shared, self.node, self.mappable, offset, byte_count)
    }

    pub(crate) fn times(&self) -> Result<[Option<u64>; 3], VfsError> {
        utf_times(&self.shared, self.node)
    }
}

pub(crate) fn utf_read(
    shared: &UtfShared,
    node: NodeRef,
    pos: &mut u32,
    buf: &mut [u8],
) -> Result<usize, VfsError> {
    if buf.is_empty() {
        return Err(VfsError::InvalidParameter);
    }
    let archive = &shared.archive;
    let mapped = u32::try_from(archive.bytes().len()).unwrap_or(u32::MAX);
    let request = u32::try_from(buf.len()).unwrap_or(u32::MAX);
    let position = *pos;
    let file_off = archive.data_start(node)?.wrapping_add(position);
    let size = archive.size(node)?;
    let end = position.wrapping_add(request).min(size);
    *pos = end;
    if file_off > mapped {
        return Err(VfsError::ReadFault);
    }
    // A wrapped end makes the reference copy past the caller's buffer; the
    // copy stops at the buffer here.
    let count = end
        .wrapping_sub(position)
        .min(mapped - file_off)
        .min(request) as usize;
    let start = file_off as usize;
    buf[..count].copy_from_slice(&archive.bytes()[start..start + count]);
    if count == 0 {
        return Err(VfsError::ReadFault);
    }
    Ok(count)
}

/// `SeekFile` of the UTF backend: a position past the node size fails and
/// leaves the position alone.
pub(crate) fn utf_seek(
    shared: &UtfShared,
    node: NodeRef,
    pos: &mut u32,
    offset: u32,
    origin: SeekOrigin,
) -> Result<u32, VfsError> {
    let size = shared.archive.size(node)?;
    let target = match origin {
        SeekOrigin::Begin => offset,
        SeekOrigin::Current => pos.wrapping_add(offset),
        SeekOrigin::End => size.wrapping_sub(offset),
    };
    if target > size {
        return Err(VfsError::SeekFailed);
    }
    *pos = target;
    Ok(target)
}

pub(crate) fn utf_map_view(
    shared: &UtfShared,
    node: NodeRef,
    mappable: bool,
    offset: u32,
    byte_count: u32,
) -> Result<&[u8], VfsError> {
    if !mappable {
        return Err(VfsError::AccessDenied);
    }
    let archive = &shared.archive;
    let size = archive.size(node)?;
    if byte_count > size.wrapping_sub(offset) {
        return Err(VfsError::AccessDenied);
    }
    let start = archive.data_start(node)?.wrapping_add(offset) as usize;
    let count = if byte_count == 0 {
        size.wrapping_sub(offset)
    } else {
        byte_count
    } as usize;
    // The reference hands out a pointer without checking the archive's end.
    archive
        .bytes()
        .get(start..)
        .and_then(|rest| rest.get(..count))
        .ok_or(VfsError::Malformed(crate::utf::UtfError::DataOutOfRange))
}

/// `GetFileTimes`: a full record's own times, the archive file's for a
/// compact one.
pub(crate) fn utf_times(shared: &UtfShared, node: NodeRef) -> Result<[Option<u64>; 3], VfsError> {
    match shared.archive.layout() {
        Layout::Full => {
            let packed = shared.archive.times(node)?.unwrap_or([0; 3]);
            Ok(packed.map(packed_to_file_time))
        }
        Layout::Compact => Ok(shared.times.map_or([None; 3], |t| t.map(Some))),
    }
}

/// A file opened with [`FileSystem::open_file`](super::FileSystem::open_file):
/// the C++'s handle. It keeps what it reads alive on its own.
#[derive(Clone, Debug)]
pub struct VfsFile {
    inner: Inner,
}

#[derive(Clone, Debug)]
enum Inner {
    Dos(DosStream),
    Utf(UtfStream),
}

impl VfsFile {
    pub(crate) fn dos(stream: DosStream) -> Self {
        Self {
            inner: Inner::Dos(stream),
        }
    }

    pub(crate) fn utf(stream: UtfStream) -> Self {
        Self {
            inner: Inner::Utf(stream),
        }
    }

    /// `Read(handle, buf, ...)`.
    ///
    /// A DOS file fails with [`VfsError::ShortRead`] unless `buf` is filled
    /// (the bytes read are still copied). A UTF node fills what it can; an
    /// empty `buf` is [`VfsError::InvalidParameter`] and a read that copies
    /// nothing is [`VfsError::ReadFault`].
    ///
    /// # Errors
    ///
    /// As above.
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        match &mut self.inner {
            Inner::Dos(s) => s.read(buf),
            Inner::Utf(s) => s.read(buf),
        }
    }

    /// `GetSize(handle)`.
    ///
    /// # Errors
    ///
    /// A UTF record outside the archive.
    pub fn size(&self) -> Result<u32, VfsError> {
        match &self.inner {
            Inner::Dos(s) => Ok(s.size()),
            Inner::Utf(s) => s.size(),
        }
    }

    /// `SeekFile(handle, offset, NULL, origin)`.
    ///
    /// # Errors
    ///
    /// [`VfsError::SeekFailed`]: an empty DOS file, a DOS position of
    /// 0xFFFFFFFF (which is still stored), or a UTF position past the size
    /// (which is not).
    pub fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        match &mut self.inner {
            Inner::Dos(s) => s.seek(offset, origin),
            Inner::Utf(s) => s.seek(offset, origin),
        }
    }

    /// `GetFilePosition(handle)`. The UTF backend never reports a position:
    /// the C++ returns 0.
    ///
    /// # Errors
    ///
    /// An empty DOS file.
    pub fn position(&self) -> Result<u32, VfsError> {
        match &self.inner {
            Inner::Dos(s) => s.position(),
            Inner::Utf(_) => Ok(0),
        }
    }

    /// `CreateMapping(handle)` and `MapFileView(..., offset, byte_count)`.
    /// The UTF backend maps only files opened from a file system that was
    /// itself made by `create_child`.
    ///
    /// # Errors
    ///
    /// [`VfsError::AccessDenied`] where the backend refuses; an error where
    /// the view would leave the data.
    pub fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError> {
        match &self.inner {
            Inner::Dos(s) => s.map_view(offset, byte_count),
            Inner::Utf(s) => s.map_view(offset, byte_count),
        }
    }

    /// `GetFileTimes(handle)`: creation, last access and last write.
    ///
    /// # Errors
    ///
    /// A UTF record outside the archive.
    pub fn times(&self) -> Result<[Option<u64>; 3], VfsError> {
        match &self.inner {
            Inner::Dos(s) => Ok(s.times()),
            Inner::Utf(s) => s.times(),
        }
    }

    /// The whole file as one read from the start returns it: a DOS file's
    /// contents, a UTF node's data cut at the end of the archive.
    ///
    /// # Errors
    ///
    /// The UTF data starts outside the archive.
    pub fn contents(&self) -> Result<&[u8], VfsError> {
        match &self.inner {
            Inner::Dos(s) => Ok(s.bytes()),
            Inner::Utf(s) => Ok(s.shared.archive.data(s.node)?),
        }
    }
}
