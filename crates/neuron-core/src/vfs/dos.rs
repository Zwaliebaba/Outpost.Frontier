//! The DOS backend (`FDosFileSystem`): a host directory, or one host file.

use super::path::{
    JOIN_CHARS, ROOT_PATH_CHARS, copy_clamped_path_suffix, copy_path, is_separator, join_root,
    join_root_buffer, seal_directory_root,
};
use super::stream::DosStream;
use super::{
    ATTRIBUTE_DIRECTORY, FileSystem, FindEntry, FsDesc, FsKind, GENERIC_WRITE, Host, SeekOrigin,
    UtfFileSystem, VfsError, VfsFile,
};
use crate::hash::c_str;

/// A host directory, or one host file (a "file instance").
#[derive(Clone, Debug)]
pub struct DosFileSystem {
    host: Host,
    /// `RootPath`: a directory's path with a final `\`, or the path a file
    /// instance was asked for.
    root_path: Vec<u8>,
    /// `PathPrefix`: the index of a directory root's final `\` when it was
    /// adopted, 0 for a file instance.
    path_prefix: usize,
    file: Option<DosStream>,
    access: u32,
}

impl DosFileSystem {
    fn directory(host: &Host, path: &[u8], access: u32) -> Self {
        let (root_path, path_prefix) = seal_directory_root(path);
        Self {
            host: host.clone(),
            root_path,
            path_prefix,
            file: None,
            access,
        }
    }

    fn file_instance(
        host: &Host,
        resolved: &[u8],
        asked: &[u8],
        access: u32,
    ) -> Result<Self, VfsError> {
        let file = host.read_file(resolved).map_err(|_| VfsError::OpenFailed)?;
        Ok(Self {
            host: host.clone(),
            root_path: copy_path(asked, ROOT_PATH_CHARS),
            path_prefix: 0,
            file: Some(DosStream::new(file.data, file.times, access)),
            access,
        })
    }

    /// The DOS factory without a parent (`CreateOnSingleton`): a directory
    /// at `desc.path` becomes a directory file system, a file a file
    /// instance. The path is relative to the host's current directory.
    ///
    /// # Errors
    ///
    /// [`VfsError::Generic`] for a request other than read / open-existing;
    /// [`VfsError::OpenFailed`] when nothing usable is at the path.
    pub fn open(host: &Host, desc: &FsDesc<'_>) -> Result<Self, VfsError> {
        if !desc.checks_pass() {
            return Err(VfsError::Generic);
        }
        match host.attributes(desc.path) {
            Ok(Some(attributes)) if attributes & ATTRIBUTE_DIRECTORY != 0 => {
                Ok(Self::directory(host, desc.path, desc.access))
            }
            Ok(Some(_)) => Self::file_instance(host, desc.path, desc.path, desc.access),
            _ => Err(VfsError::OpenFailed),
        }
    }

    /// The root path (`RootPath`).
    #[must_use]
    pub fn root_path(&self) -> &[u8] {
        &self.root_path
    }

    /// Whether this is a file instance.
    #[must_use]
    pub fn is_file_instance(&self) -> bool {
        self.file.is_some()
    }

    fn join(&self, rel: &[u8]) -> Option<Vec<u8>> {
        join_root(&self.root_path, rel, JOIN_CHARS)
    }

    fn stream(&self) -> Result<&DosStream, VfsError> {
        self.file.as_ref().ok_or(VfsError::InvalidHandle)
    }
}

impl FileSystem for DosFileSystem {
    fn kind(&self) -> FsKind {
        if self.file.is_some() {
            FsKind::DosFile
        } else {
            FsKind::DosDirectory
        }
    }

    /// `CreateRelative`: `desc.path` joined to the root path. A directory
    /// becomes a directory file system; a file opened as `"DOS"` becomes a
    /// file instance keeping `desc.path` as its root path; any other file
    /// goes to the UTF factory with this directory as the parent.
    fn create_child(&self, desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
        if !desc.checks_pass() || self.file.is_some() {
            return Err(VfsError::Generic);
        }
        let resolved = self.join(desc.path).ok_or(VfsError::Generic)?;
        let attributes = self
            .host
            .attributes(&resolved)
            .ok()
            .flatten()
            .ok_or(VfsError::Generic)?;
        if attributes & ATTRIBUTE_DIRECTORY != 0 {
            // The C++ never sets a relative directory's access flags.
            return Ok(Box::new(Self::directory(&self.host, &resolved, 0)));
        }
        if desc
            .implementation
            .is_some_and(|name| c_str(name) == b"DOS")
        {
            return Ok(Box::new(Self::file_instance(
                &self.host,
                &resolved,
                desc.path,
                desc.access,
            )?));
        }
        let dir_path = self.file_sys_path(JOIN_CHARS);
        match UtfFileSystem::open_in_directory(&self.host, &dir_path, desc) {
            Ok(archive) => Ok(Box::new(archive)),
            // The C++ then falls through to the DOS factory, which wraps
            // this directory in a file system that cannot read.
            Err(_) => Err(VfsError::NotAnArchive),
        }
    }

    /// `OpenByDesc`: a file (not a directory) at `desc.path` joined to the
    /// root path.
    fn open_file(&self, desc: &FsDesc<'_>) -> Result<VfsFile, VfsError> {
        if self.file.is_some() {
            return Err(VfsError::FileNotFound);
        }
        let resolved = self.join(desc.path).ok_or(VfsError::FileNotFound)?;
        match self.host.attributes(&resolved) {
            Ok(Some(attributes)) if attributes & ATTRIBUTE_DIRECTORY == 0 => {}
            _ => return Err(VfsError::FileNotFound),
        }
        let file = self
            .host
            .read_file(&resolved)
            .map_err(|_| VfsError::FileNotFound)?;
        // CreateMapping tests the file system's access flags, not the
        // descriptor's.
        Ok(VfsFile::dos(DosStream::new(
            file.data,
            file.times,
            self.access,
        )))
    }

    /// The root path becomes the joined directory with a final `\`; the
    /// prefix stays, so [`FileSystem::file_sys_path`] and
    /// [`FileSystem::resolve_path`] keep cutting at the old position.
    fn open_node(&mut self, name: &[u8]) -> Result<(), VfsError> {
        if self.file.is_some() {
            return Err(VfsError::AccessDenied);
        }
        let resolved = self.join(name).ok_or(VfsError::BadPathname)?;
        match self.host.attributes(&resolved) {
            Ok(Some(attributes)) if attributes & ATTRIBUTE_DIRECTORY != 0 => {}
            _ => return Err(VfsError::BadPathname),
        }
        self.root_path = resolved;
        if !self.root_path.last().copied().is_some_and(is_separator) {
            self.root_path.push(b'\\');
        }
        Ok(())
    }

    /// The host attributes of the joined path. A missing path is
    /// [`VfsError::FileNotFound`]; the C++ returns `INVALID_FILE_ATTRIBUTES`
    /// with the system's error.
    fn path_attributes(&self, path: &[u8]) -> Result<u32, VfsError> {
        if self.file.is_some() {
            return Err(VfsError::AccessDenied);
        }
        let resolved = self.join(path).ok_or(VfsError::BadPathname)?;
        self.host
            .attributes(&resolved)?
            .ok_or(VfsError::FileNotFound)
    }

    /// The reconstruction's `FindFirst` fails whenever the pattern joins
    /// (it tests the join's result the wrong way round), so a directory
    /// file system finds nothing. Where the join fails the C++ passes the
    /// partial path to the system, which finds no file either.
    fn find(&self, pattern: &[u8]) -> Result<Vec<FindEntry>, VfsError> {
        if self.file.is_some() {
            return Err(VfsError::FileNotFound);
        }
        match self.join(pattern) {
            Some(_) => Err(VfsError::BadPathname),
            None => Err(VfsError::FileNotFound),
        }
    }

    /// Joins to the root path from the prefix on: `\` plus whatever
    /// [`FileSystem::open_node`] added for a directory, the whole path for
    /// a file instance.
    fn resolve_path(&self, src: &[u8], dest_chars: usize) -> (bool, Vec<u8>) {
        let root = &self.root_path[self.path_prefix.min(self.root_path.len())..];
        join_root_buffer(root, src, dest_chars)
    }

    /// The root path cut at the prefix (a directory without its final `\`),
    /// or whole for a file instance; at most `max_chars - 1` bytes.
    fn file_sys_path(&self, max_chars: usize) -> Vec<u8> {
        let count = (self.root_path.len() + 1).min(max_chars);
        if count == 0 {
            return Vec::new();
        }
        if self.path_prefix != 0 && count > self.path_prefix {
            return self.root_path[..self.path_prefix].to_vec();
        }
        self.root_path[..count - 1].to_vec()
    }

    fn current_path(&self, max_chars: usize) -> Result<Vec<u8>, VfsError> {
        if self.file.is_some() {
            return Err(VfsError::AccessDenied);
        }
        let suffix = &self.root_path[self.path_prefix.min(self.root_path.len())..];
        Ok(copy_clamped_path_suffix(suffix, max_chars).0)
    }

    fn access(&self) -> u32 {
        self.access
    }

    /// A directory has no stream. The C++ then returns success without
    /// writing the byte count (divergence register).
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        self.file.as_mut().ok_or(VfsError::InvalidHandle)?.read(buf)
    }

    fn size(&self) -> Result<u32, VfsError> {
        Ok(self.stream()?.size())
    }

    fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        self.file
            .as_mut()
            .ok_or(VfsError::SeekFailed)?
            .seek(offset, origin)
    }

    fn position(&self) -> Result<u32, VfsError> {
        self.file.as_ref().ok_or(VfsError::SeekFailed)?.position()
    }

    fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError> {
        if self.access & GENERIC_WRITE != 0 {
            return Err(VfsError::AccessDenied);
        }
        self.stream()?.map_view(offset, byte_count)
    }
}
