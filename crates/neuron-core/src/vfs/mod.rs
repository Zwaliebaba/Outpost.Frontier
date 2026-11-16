//! File systems (work package P1.3): directories on the host, UTF
//! archives, memory buffers, and the search path over several roots.
//!
//! Ported from `CppSource/ReadFile/ReadFile.cpp` (`FDosFileSystem`,
//! `FUtfFileSystem`, `FMemFileSystem`, `FSearchPathInstance`). The C++
//! exposes one 38-slot COM-style interface whose files are integer handles
//! (object addresses). Here a file system is a [`FileSystem`] trait object
//! and an opened file a [`VfsFile`] value that owns what it reads; the
//! component manager, reference counts, critical sections and DLL glue are
//! gone (decision T9). Path rules, lookup order, results and error codes
//! follow the C++.
//!
//! * [`create_file_system`] is the component manager's "FileSystem"
//!   request: a UTF archive if the path opens as one, else a directory or
//!   a single file on the host.
//! * [`FileSystem::create_child`] is `IFileSystem::CreateInstance`: a
//!   subdirectory, a file, or a UTF archive inside a directory; a node
//!   inside a UTF archive.
//! * [`SearchPath`] resolves a request over an ordered list of roots.
//! * [`MemFileSystem`] is the growable memory buffer.
//!
//! Paths are byte strings ending at their first NUL; see [`path`].

pub mod path;

mod dos;
mod host;
mod mem;
mod search;
mod stream;
mod utf_fs;

pub use dos::DosFileSystem;
pub use host::{
    ATTRIBUTE_DIRECTORY, ATTRIBUTE_NORMAL, ATTRIBUTE_READONLY, Host, HostEntry, HostFile,
};
pub use mem::{MemFileSystem, MemSource};
pub use search::{SEARCH_PATH_CHARS, SearchPath, split_search_entry, split_search_path};
pub use stream::VfsFile;
pub use utf_fs::UtfFileSystem;

use crate::utf::{NodeRef, UtfError};

/// `GENERIC_READ`.
pub const GENERIC_READ: u32 = 0x8000_0000;
/// `GENERIC_WRITE`.
pub const GENERIC_WRITE: u32 = 0x4000_0000;
/// `OPEN_EXISTING`.
pub const OPEN_EXISTING: u32 = 3;
/// `MAX_PATH`; find results keep at most `MAX_PATH - 1` name bytes.
pub const MAX_PATH: usize = 260;

/// Why an operation failed. Each variant names the code the C++ stores as
/// its last error or returns as a result; [`VfsError::code`] gives it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VfsError {
    /// Last error 2 (`ERROR_FILE_NOT_FOUND`).
    FileNotFound,
    /// Last error 5 (`ERROR_ACCESS_DENIED`): an operation the backend
    /// refuses.
    AccessDenied,
    /// Last error 6 (`ERROR_INVALID_HANDLE`).
    InvalidHandle,
    /// Last error 0x12 (`ERROR_NO_MORE_FILES`).
    NoMoreFiles,
    /// Last error 0x1E (`ERROR_READ_FAULT`): a UTF read that returns no
    /// bytes.
    ReadFault,
    /// Last error 0x57 (`ERROR_INVALID_PARAMETER`).
    InvalidParameter,
    /// Last error 0xA1 (`ERROR_BAD_PATHNAME`): a path that does not join or
    /// does not name the node it must.
    BadPathname,
    /// A DOS read that returned fewer bytes than asked (last error
    /// 0xFFFFFFFF). The bytes read are in the caller's buffer.
    ShortRead {
        /// Bytes read.
        read: usize,
    },
    /// A seek the backend refuses (result -1).
    SeekFailed,
    /// The memory backend's failure, which sets no error code.
    Failed,
    /// `CreateInstance` result -1 (`GR_GENERIC`).
    Generic,
    /// `CreateInstance` result -3 (`GR_INTERFACE_UNSUPPORTED`).
    Unsupported,
    /// `CreateInstance` result -6: the file could not be opened as asked.
    OpenFailed,
    /// A file inside a directory that is neither opened as `"DOS"` nor a
    /// UTF archive. The reference returns success with a file system that
    /// wraps the directory and whose operations fail or return undefined
    /// values (divergence register).
    NotAnArchive,
    /// The archive is malformed where the reference reads outside it.
    Malformed(UtfError),
    /// A host error other than a missing file.
    Host(std::io::ErrorKind),
    /// A path that is not UTF-8 cannot name a host file.
    NonUtf8Path,
}

impl VfsError {
    /// The C++ code: the last error for operation failures, the
    /// `GENRESULT` bits for [`VfsError::Generic`], [`VfsError::Unsupported`]
    /// and [`VfsError::OpenFailed`], 0 where the C++ has none.
    #[must_use]
    pub const fn code(self) -> u32 {
        match self {
            Self::FileNotFound => 2,
            Self::AccessDenied => 5,
            Self::InvalidHandle => 6,
            Self::NoMoreFiles => 0x12,
            Self::ReadFault => 0x1E,
            Self::InvalidParameter => 0x57,
            Self::BadPathname => 0xA1,
            Self::ShortRead { .. } | Self::SeekFailed | Self::Generic => 0xFFFF_FFFF,
            Self::Unsupported => 0xFFFF_FFFD,
            Self::OpenFailed => 0xFFFF_FFFA,
            Self::Failed
            | Self::NotAnArchive
            | Self::Malformed(_)
            | Self::Host(_)
            | Self::NonUtf8Path => 0,
        }
    }
}

impl core::fmt::Display for VfsError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::FileNotFound => f.write_str("file not found"),
            Self::AccessDenied => f.write_str("operation not supported by this file system"),
            Self::InvalidHandle => f.write_str("invalid handle"),
            Self::NoMoreFiles => f.write_str("no more files"),
            Self::ReadFault => f.write_str("read past the end of the file"),
            Self::InvalidParameter => f.write_str("invalid parameter"),
            Self::BadPathname => f.write_str("bad path name"),
            Self::ShortRead { read } => write!(f, "short read ({read} bytes)"),
            Self::SeekFailed => f.write_str("seek failed"),
            Self::Failed => f.write_str("operation failed"),
            Self::Generic => f.write_str("file system not created"),
            Self::Unsupported => f.write_str("request not supported"),
            Self::OpenFailed => f.write_str("file could not be opened"),
            Self::NotAnArchive => f.write_str("file is not a UTF archive"),
            Self::Malformed(e) => write!(f, "malformed archive: {e}"),
            Self::Host(kind) => write!(f, "host file system: {kind}"),
            Self::NonUtf8Path => f.write_str("path is not UTF-8"),
        }
    }
}

impl std::error::Error for VfsError {}

impl From<UtfError> for VfsError {
    fn from(error: UtfError) -> Self {
        Self::Malformed(error)
    }
}

/// Which backend a file system is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FsKind {
    /// A host directory.
    DosDirectory,
    /// One host file.
    DosFile,
    /// A UTF archive or a node in one.
    Utf,
    /// A memory buffer.
    Memory,
}

/// The origin of a seek (`FILE_BEGIN`, `FILE_CURRENT`, `FILE_END`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SeekOrigin {
    /// From the start.
    Begin,
    /// From the current position.
    Current,
    /// The C++ subtracts the offset from the end (`size - offset`).
    End,
}

/// A record a UTF find returned, usable instead of a path to open it. It
/// stands for the find handle the C++ passes in the descriptor's +0x30
/// field.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FindToken {
    archive: usize,
    node: NodeRef,
}

impl FindToken {
    /// The record.
    #[must_use]
    pub fn node(self) -> NodeRef {
        self.node
    }
}

/// One `WIN32_FIND_DATAA` a find returned.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FindData {
    /// The attributes: a full UTF record's raw flags, 0x10 or 0 for a
    /// compact one.
    pub attributes: u32,
    /// Creation, last access and last write times as `FILETIME` counts.
    /// `None` where the C++ conversion fails and leaves the caller's
    /// previous value.
    pub times: [Option<u64>; 3],
    /// The size: a full record's size field, a compact file's size, 0 for
    /// a compact directory.
    pub size: u32,
    /// The name, at most [`MAX_PATH`] - 1 bytes.
    pub name: Vec<u8>,
}

impl FindData {
    /// Whether the directory bit is set.
    #[must_use]
    pub fn is_dir(&self) -> bool {
        self.attributes & ATTRIBUTE_DIRECTORY != 0
    }
}

/// One find result.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FindEntry {
    /// What `FindFirst` / `FindNext` wrote.
    pub data: FindData,
    /// Opens the entry itself, as the find handle does.
    pub token: FindToken,
}

/// The `FileSystem` descriptor callers fill (`FFileSystemDesc`, size 0x34).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FsDesc<'a> {
    /// `"DOS"`, `"UTF"`, another name, or none.
    pub implementation: Option<&'a [u8]>,
    /// The path.
    pub path: &'a [u8],
    /// The access mask; the DOS and UTF backends accept only
    /// [`GENERIC_READ`].
    pub access: u32,
    /// The creation disposition; only [`OPEN_EXISTING`] is accepted.
    pub creation: u32,
    /// The find handle (+0x30), or `None` for -1.
    pub found: Option<FindToken>,
}

impl<'a> FsDesc<'a> {
    /// A read request for `path` with no implementation named, as callers
    /// fill it.
    #[must_use]
    pub const fn read(path: &'a [u8]) -> Self {
        Self {
            implementation: None,
            path,
            access: GENERIC_READ,
            creation: OPEN_EXISTING,
            found: None,
        }
    }

    /// The same request naming `implementation`.
    #[must_use]
    pub const fn with_implementation(mut self, implementation: &'a [u8]) -> Self {
        self.implementation = Some(implementation);
        self
    }

    /// The same request for a found entry.
    #[must_use]
    pub const fn with_found(mut self, token: FindToken) -> Self {
        self.found = Some(token);
        self
    }

    fn checks_pass(&self) -> bool {
        self.access == GENERIC_READ && self.creation == OPEN_EXISTING
    }
}

/// A file system. Each one also has a stream of its own, the C++'s handle
/// 0: the file of a file instance, the node of a UTF node, the buffer of a
/// memory file system.
pub trait FileSystem: core::fmt::Debug {
    /// The backend.
    fn kind(&self) -> FsKind;

    /// `CreateInstance` with a `FileSystem` descriptor: a file system for
    /// `desc.path` (or `desc.found`) below this one.
    ///
    /// # Errors
    ///
    /// The backend's result code.
    fn create_child(&self, desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError>;

    /// `OpenByDesc`: opens a file below this file system.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn open_file(&self, desc: &FsDesc<'_>) -> Result<VfsFile, VfsError>;

    /// `OpenNode`: makes `name` (joined to the current path) the current
    /// directory.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn open_node(&mut self, name: &[u8]) -> Result<(), VfsError>;

    /// `GetPathAttributes`: 0x10 for a directory, other attribute bits for a
    /// file. Where the C++ fails, the DOS backend returns
    /// `INVALID_FILE_ATTRIBUTES` (all bits set) for a missing host path and
    /// 0 otherwise; the UTF backend returns 0.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn path_attributes(&self, path: &[u8]) -> Result<u32, VfsError>;

    /// A whole `FindFirst` / `FindNext` loop. An empty list is the C++'s
    /// `FindFirst` failing with [`VfsError::NoMoreFiles`].
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn find(&self, pattern: &[u8]) -> Result<Vec<FindEntry>, VfsError>;

    /// `ResolvePath(dest, src, dest_chars)` on a zero-filled destination:
    /// the result and the C string the destination holds.
    fn resolve_path(&self, src: &[u8], dest_chars: usize) -> (bool, Vec<u8>);

    /// `GetFileSysPath(out, max_chars)`: the bytes written before the NUL,
    /// or all `max_chars` bytes when the C++ leaves them unterminated.
    fn file_sys_path(&self, max_chars: usize) -> Vec<u8>;

    /// `GetCurrentPath(max_chars, out)`.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn current_path(&self, max_chars: usize) -> Result<Vec<u8>, VfsError>;

    /// `GetAccessFlags`.
    fn access(&self) -> u32;

    /// `Read(0, ...)` on the own stream.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError>;

    /// `GetSize(0)`.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn size(&self) -> Result<u32, VfsError>;

    /// `SeekFile(0, offset, NULL, origin)`: the new position.
    ///
    /// # Errors
    ///
    /// The backend refuses the position.
    fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError>;

    /// `GetFilePosition(0)`.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn position(&self) -> Result<u32, VfsError>;

    /// `CreateMapping(0)` and `MapFileView(..., offset, byte_count)`: the
    /// own stream's bytes from `offset`, `byte_count` of them or all the
    /// rest for 0.
    ///
    /// # Errors
    ///
    /// The backend's last error.
    fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError>;

    /// `Write(0, ...)`; only the memory backend writes.
    ///
    /// # Errors
    ///
    /// [`VfsError::AccessDenied`] on the DOS and UTF backends.
    fn write(&mut self, data: &[u8]) -> Result<usize, VfsError> {
        let _ = data;
        Err(VfsError::AccessDenied)
    }

    /// `SetEndOfFile(0)`; only the memory backend writes.
    ///
    /// # Errors
    ///
    /// [`VfsError::AccessDenied`] on the DOS and UTF backends.
    fn set_end_of_file(&mut self) -> Result<(), VfsError> {
        Err(VfsError::AccessDenied)
    }
}

/// The component manager's `CreateInstance` for a `FileSystem` descriptor
/// without a parent: the factories in registration order, memory (which
/// refuses this descriptor size), UTF, then DOS.
///
/// * UTF: refuses an implementation other than `"UTF"` and requests other
///   than read / open-existing, then opens `desc.path` (relative to the
///   host's current directory) as an archive.
/// * DOS: refuses requests other than read / open-existing; a directory
///   becomes a directory file system, a file a file instance. Its result
///   is returned when both fail.
///
/// # Errors
///
/// The DOS factory's result: [`VfsError::Generic`] for a refused request,
/// [`VfsError::OpenFailed`] when nothing usable is at the path.
pub fn create_file_system(host: &Host, desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
    if let Ok(archive) = UtfFileSystem::open(host, desc) {
        return Ok(Box::new(archive));
    }
    Ok(Box::new(DosFileSystem::open(host, desc)?))
}

#[cfg(test)]
mod tests;
