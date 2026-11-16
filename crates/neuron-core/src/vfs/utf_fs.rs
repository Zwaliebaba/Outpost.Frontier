//! The UTF backend (`FUtfFileSystem`): an archive, or a node in one.

use std::sync::Arc;

use super::path::{
    JOIN_CHARS, copy_clamped_path_suffix, is_separator, join_root, join_root_buffer, make_path,
};
use super::stream::{UtfStream, utf_map_view, utf_read, utf_seek};
use super::{
    ATTRIBUTE_DIRECTORY, FileSystem, FindData, FindEntry, FindToken, FsDesc, FsKind, Host,
    MAX_PATH, MemFileSystem, SeekOrigin, VfsError, VfsFile,
};
use crate::hash::c_str;
use crate::utf::{Layout, NodeRef, UtfArchive, packed_to_file_time};

/// Longest path an archive or node keeps as its own (`SetOwnPath`).
const OWN_PATH_LIMIT: usize = 262;

/// What every file system made from one archive shares.
#[derive(Debug)]
pub(crate) struct UtfShared {
    pub(crate) archive: UtfArchive,
    /// The archive file's creation, access and write times, which compact
    /// records report as their own. `None` for an archive made from memory.
    pub(crate) times: Option<[u64; 3]>,
    /// The host the archive was opened from; see `create_child`.
    pub(crate) host: Option<Host>,
}

/// A UTF archive, or a node of one made with
/// [`FileSystem::create_child`]. Every file system made from one archive
/// shares its bytes.
#[derive(Clone, Debug)]
pub struct UtfFileSystem {
    shared: Arc<UtfShared>,
    /// The record handle 0 names: the tree root of an archive, the node of a
    /// clone.
    root: NodeRef,
    root_pos: u32,
    /// Made by `create_child` (the C++ has a parent and an archive owner).
    clone: bool,
    own_path: Vec<u8>,
    own_path_prefix: usize,
    current_path: Vec<u8>,
    open_node: Option<NodeRef>,
    access: u32,
}

fn refuses(desc: &FsDesc<'_>) -> bool {
    desc.implementation
        .is_some_and(|name| c_str(name) != b"UTF")
        || !desc.checks_pass()
}

impl UtfFileSystem {
    fn with_archive(
        archive: UtfArchive,
        times: Option<[u64; 3]>,
        host: Option<Host>,
        path: &[u8],
        access: u32,
    ) -> Self {
        let mut fs = Self {
            shared: Arc::new(UtfShared {
                archive,
                times,
                host,
            }),
            root: NodeRef::ROOT,
            root_pos: 0,
            clone: false,
            own_path: Vec::new(),
            own_path_prefix: 0,
            current_path: b"\\".to_vec(),
            open_node: None,
            access,
        };
        fs.set_own_path(path, None);
        fs
    }

    fn from_file(
        host: &Host,
        bytes: Vec<u8>,
        times: [u64; 3],
        desc: &FsDesc<'_>,
    ) -> Result<Self, VfsError> {
        let archive = UtfArchive::parse(bytes).map_err(|_| VfsError::OpenFailed)?;
        Ok(Self::with_archive(
            archive,
            Some(times),
            Some(host.clone()),
            desc.path,
            desc.access,
        ))
    }

    /// The UTF factory without a parent: `desc.path`, relative to the
    /// host's current directory, opened as an archive.
    ///
    /// # Errors
    ///
    /// [`VfsError::Generic`] for an implementation other than `"UTF"`, a
    /// request other than read / open-existing, or a file that cannot be
    /// opened; [`VfsError::OpenFailed`] for a file that is not an archive.
    pub fn open(host: &Host, desc: &FsDesc<'_>) -> Result<Self, VfsError> {
        if refuses(desc) {
            return Err(VfsError::Generic);
        }
        let file = host.read_file(desc.path).map_err(|_| VfsError::Generic)?;
        Self::from_file(host, file.data, file.times, desc)
    }

    /// The UTF factory with a directory as the parent: the archive at
    /// `_makepath(dir_path, desc.path)`, where `dir_path` is the directory's
    /// `GetFileSysPath`.
    pub(crate) fn open_in_directory(
        host: &Host,
        dir_path: &[u8],
        desc: &FsDesc<'_>,
    ) -> Result<Self, VfsError> {
        if refuses(desc) {
            return Err(VfsError::Generic);
        }
        let made = make_path(dir_path, desc.path);
        let file = host.read_file(&made).map_err(|_| VfsError::Generic)?;
        Self::from_file(host, file.data, file.times, desc)
    }

    /// The UTF factory with a memory file system as the parent: the
    /// buffer it hands out (its allocated size, not its logical size) as an
    /// archive. The bytes are copied; the C++ reads the buffer in place.
    ///
    /// A compact archive fails here as in the C++, which asks the missing
    /// file handle for the times compact records report.
    ///
    /// # Errors
    ///
    /// [`VfsError::Generic`] for a refused request or an empty buffer;
    /// [`VfsError::OpenFailed`] for bytes that are not a full archive.
    pub fn open_memory(memory: &MemFileSystem, desc: &FsDesc<'_>) -> Result<Self, VfsError> {
        if refuses(desc) {
            return Err(VfsError::Generic);
        }
        let buffer = memory.buffer();
        if buffer.is_empty() {
            return Err(VfsError::Generic);
        }
        let archive = UtfArchive::parse(buffer.to_vec()).map_err(|_| VfsError::OpenFailed)?;
        if archive.layout() == Layout::Compact {
            return Err(VfsError::OpenFailed);
        }
        Ok(Self::with_archive(
            archive,
            None,
            None,
            desc.path,
            desc.access,
        ))
    }

    /// An archive from bytes, with no file behind it: compact records then
    /// report no times. `path` becomes the archive's own path.
    ///
    /// # Errors
    ///
    /// The bytes are not an archive.
    pub fn from_bytes(bytes: impl Into<Arc<[u8]>>, path: &[u8]) -> Result<Self, VfsError> {
        let archive = UtfArchive::parse(bytes)?;
        Ok(Self::with_archive(
            archive,
            None,
            None,
            path,
            super::GENERIC_READ,
        ))
    }

    /// The archive.
    #[must_use]
    pub fn archive(&self) -> &UtfArchive {
        &self.shared.archive
    }

    /// The record handle 0 names: the root of an archive, the node of a
    /// clone.
    #[must_use]
    pub fn root(&self) -> NodeRef {
        self.root
    }

    /// The node the last [`FileSystem::open_node`] opened.
    #[must_use]
    pub fn open_node_ref(&self) -> Option<NodeRef> {
        self.open_node
    }

    /// The current path `open_node` keeps and relative paths join to.
    #[must_use]
    pub fn current_dir(&self) -> &[u8] {
        &self.current_path
    }

    /// `SetOwnPath(path, handle)`: the path cut to 262 bytes; a directory
    /// node's path ends in exactly one `\`.
    fn set_own_path(&mut self, path: &[u8], is_dir: Option<bool>) {
        let path = c_str(path);
        let len = path.len().min(OWN_PATH_LIMIT);
        self.own_path = path[..len].to_vec();
        self.own_path_prefix = len;
        if is_dir == Some(true) {
            if self.own_path_prefix > 0 && is_separator(self.own_path[self.own_path_prefix - 1]) {
                self.own_path_prefix -= 1;
            }
            self.own_path.truncate(self.own_path_prefix);
            self.own_path.push(b'\\');
        }
    }

    fn token(&self, node: NodeRef) -> FindToken {
        FindToken {
            archive: Arc::as_ptr(&self.shared).cast::<()>().addr(),
            node,
        }
    }

    fn token_node(&self, token: FindToken) -> Option<NodeRef> {
        (token.archive == Arc::as_ptr(&self.shared).cast::<()>().addr()).then_some(token.node)
    }

    fn find_data(&self, node: NodeRef) -> Result<FindData, VfsError> {
        let archive = &self.shared.archive;
        let name = archive.name(node)?;
        let name = name[..name.len().min(MAX_PATH - 1)].to_vec();
        Ok(match archive.layout() {
            Layout::Full => FindData {
                attributes: archive.attributes(node)?,
                times: archive
                    .times(node)?
                    .unwrap_or([0; 3])
                    .map(packed_to_file_time),
                size: archive.size(node)?,
                name,
            },
            Layout::Compact => {
                let is_dir = archive.is_dir(node)?;
                FindData {
                    attributes: if is_dir { ATTRIBUTE_DIRECTORY } else { 0 },
                    times: self.shared.times.map_or([None; 3], |t| t.map(Some)),
                    size: if is_dir { 0 } else { archive.size(node)? },
                    name,
                }
            }
        })
    }

    /// The walker's `Open`: the found record, or `path` walked from `base`.
    fn locate(
        &self,
        base: NodeRef,
        path: &[u8],
        found: Option<FindToken>,
    ) -> Result<Option<NodeRef>, VfsError> {
        match found {
            Some(token) => Ok(self.token_node(token)),
            None => Ok(self.shared.archive.walk(base, path)?),
        }
    }
}

impl FileSystem for UtfFileSystem {
    fn kind(&self) -> FsKind {
        FsKind::Utf
    }

    /// `CloneAtNode`: `desc.path` joined to the current path (the join's
    /// result is not checked) and walked from this file system's node, or
    /// the found record. The new file system's own path is `desc.path`, its
    /// access this one's masked with `desc.access`.
    ///
    /// The C++ decides between cloning and opening a new archive by testing
    /// whether the own path is exactly `\`, which is how it recognises its
    /// registered factory. A node made with the path `""`, `\` or `/` has
    /// that own path, so it opens `desc.path` as a new archive relative to
    /// the host's current directory instead, as the factory does. An
    /// archive made from memory has no host and fails there.
    fn create_child(&self, desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
        if refuses(desc) {
            return Err(VfsError::Generic);
        }
        if self.own_path == b"\\" {
            return match &self.shared.host {
                Some(host) => Ok(Box::new(Self::open(host, desc)?)),
                None => Err(VfsError::Generic),
            };
        }
        let (_, dest) = join_root_buffer(&self.current_path, desc.path, JOIN_CHARS);
        let node = self
            .locate(self.root, &dest, desc.found)?
            .ok_or(VfsError::Generic)?;
        let is_dir = self.shared.archive.is_dir(node)?;
        let mut child = Self {
            shared: Arc::clone(&self.shared),
            root: node,
            root_pos: 0,
            clone: true,
            own_path: Vec::new(),
            own_path_prefix: 0,
            current_path: b"\\".to_vec(),
            open_node: None,
            access: self.access & desc.access,
        };
        child.set_own_path(desc.path, Some(is_dir));
        Ok(Box::new(child))
    }

    /// After [`FileSystem::open_node`], `desc.path` is walked from the open
    /// node as it is; before, it is joined to the current path first.
    fn open_file(&self, desc: &FsDesc<'_>) -> Result<VfsFile, VfsError> {
        let node = match self.open_node {
            Some(base) => self.locate(base, desc.path, desc.found)?,
            None => {
                let dest = join_root(&self.current_path, desc.path, JOIN_CHARS)
                    .ok_or(VfsError::BadPathname)?;
                self.locate(self.root, &dest, desc.found)?
            }
        }
        .ok_or(VfsError::FileNotFound)?;
        Ok(VfsFile::utf(UtfStream {
            shared: Arc::clone(&self.shared),
            node,
            pos: 0,
            mappable: self.clone,
        }))
    }

    fn open_node(&mut self, name: &[u8]) -> Result<(), VfsError> {
        if !self.shared.archive.is_dir(self.root)? {
            return Err(VfsError::AccessDenied);
        }
        let dest = join_root(&self.current_path, name, JOIN_CHARS).ok_or(VfsError::BadPathname)?;
        let node = self
            .shared
            .archive
            .walk(self.root, &dest)?
            .ok_or(VfsError::BadPathname)?;
        self.open_node = Some(node);
        self.current_path = dest;
        if !self.current_path.last().copied().is_some_and(is_separator) {
            self.current_path.push(b'\\');
        }
        Ok(())
    }

    fn path_attributes(&self, path: &[u8]) -> Result<u32, VfsError> {
        let dest = join_root(&self.current_path, path, JOIN_CHARS).ok_or(VfsError::BadPathname)?;
        let node = self
            .shared
            .archive
            .walk(self.root, &dest)?
            .ok_or(VfsError::BadPathname)?;
        Ok(if self.shared.archive.is_dir(node)? {
            ATTRIBUTE_DIRECTORY
        } else {
            0
        })
    }

    /// After [`FileSystem::open_node`] the pattern is used as it is, from
    /// the open node; before, it is joined to the current path.
    fn find(&self, pattern: &[u8]) -> Result<Vec<FindEntry>, VfsError> {
        let (base, pattern) = match self.open_node {
            Some(base) => (base, c_str(pattern).to_vec()),
            None => (
                self.root,
                join_root(&self.current_path, pattern, JOIN_CHARS).ok_or(VfsError::BadPathname)?,
            ),
        };
        let nodes = self
            .shared
            .archive
            .find(base, &pattern)?
            .ok_or(VfsError::BadPathname)?;
        nodes
            .into_iter()
            .map(|node| {
                Ok(FindEntry {
                    data: self.find_data(node)?,
                    token: self.token(node),
                })
            })
            .collect()
    }

    fn resolve_path(&self, src: &[u8], dest_chars: usize) -> (bool, Vec<u8>) {
        join_root_buffer(&self.current_path, src, dest_chars)
    }

    fn file_sys_path(&self, max_chars: usize) -> Vec<u8> {
        self.own_path[..self.own_path.len().min(max_chars)].to_vec()
    }

    /// Only a clone answers: the end of its own path, through
    /// `CopyClampedPathSuffix`.
    fn current_path(&self, max_chars: usize) -> Result<Vec<u8>, VfsError> {
        if !self.clone {
            return Err(VfsError::AccessDenied);
        }
        let suffix = &self.own_path[self.own_path_prefix.min(self.own_path.len())..];
        Ok(copy_clamped_path_suffix(suffix, max_chars).0)
    }

    fn access(&self) -> u32 {
        self.access
    }

    fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        utf_read(&self.shared, self.root, &mut self.root_pos, buf)
    }

    fn size(&self) -> Result<u32, VfsError> {
        Ok(self.shared.archive.size(self.root)?)
    }

    fn seek(&mut self, offset: u32, origin: SeekOrigin) -> Result<u32, VfsError> {
        utf_seek(&self.shared, self.root, &mut self.root_pos, offset, origin)
    }

    /// The C++ refuses handle 0 (error 5).
    fn position(&self) -> Result<u32, VfsError> {
        Err(VfsError::AccessDenied)
    }

    fn map_view(&self, offset: u32, byte_count: u32) -> Result<&[u8], VfsError> {
        utf_map_view(&self.shared, self.root, self.clone, offset, byte_count)
    }
}
