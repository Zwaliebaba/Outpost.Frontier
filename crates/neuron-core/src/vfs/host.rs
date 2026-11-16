//! The host file system under the DOS backend.
//!
//! The reference passes Windows paths to `GetFileAttributesA` and
//! `CreateFileA`. Here those paths go through `std::fs`:
//!
//! * Both separators split components; empty and `.` components are
//!   dropped and `..` removes the previous component without looking at
//!   the disk, as Win32 path normalisation does.
//! * A relative path starts at the [`Host`]'s current directory, an
//!   explicit value instead of the process-wide one. A path with one
//!   leading separator starts at the root of that directory's drive.
//! * Drive-letter and UNC paths are used as they are on Windows and are
//!   not found on other hosts.
//! * Lookup is case-insensitive. Windows file systems are, and the game's
//!   data refers to its files in any case. On a case-sensitive host a
//!   component that does not exist as spelled is matched against the
//!   directory's entries with ASCII case folding; of several matches the
//!   first in byte order wins.
//! * A path that ends in a separator names a directory only.
//!
//! Paths must be UTF-8. Other Win32 rules (trailing dots and spaces,
//! device names, short names, the 260-character limit) are not reproduced
//! (divergence register).

use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

use super::VfsError;
use super::path::is_separator;
use crate::hash::c_str;
use crate::utf::system_time_to_file_time;

/// `FILE_ATTRIBUTE_READONLY`.
pub const ATTRIBUTE_READONLY: u32 = 0x01;
/// `FILE_ATTRIBUTE_DIRECTORY`.
pub const ATTRIBUTE_DIRECTORY: u32 = 0x10;
/// `FILE_ATTRIBUTE_NORMAL`.
pub const ATTRIBUTE_NORMAL: u32 = 0x80;

/// Resolves the DOS backend's Windows-style paths on the host.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Host {
    cwd: PathBuf,
}

/// What a path names on the host.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostEntry {
    /// The host path found.
    pub path: PathBuf,
    /// Whether it is a directory.
    pub is_dir: bool,
    /// Whether it is read-only.
    pub readonly: bool,
}

/// A file read whole, with its creation, access and write times as
/// `FILETIME` counts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostFile {
    /// The contents.
    pub data: Vec<u8>,
    /// Creation, last access and last write times.
    pub times: [u64; 3],
}

fn host_error(error: &io::Error) -> VfsError {
    match error.kind() {
        io::ErrorKind::NotFound => VfsError::FileNotFound,
        kind => VfsError::Host(kind),
    }
}

impl Host {
    /// A host whose relative paths start at `cwd`.
    #[must_use]
    pub fn new(cwd: impl Into<PathBuf>) -> Self {
        Self { cwd: cwd.into() }
    }

    /// A host whose relative paths start at the process's current
    /// directory.
    ///
    /// # Errors
    ///
    /// The current directory cannot be read.
    pub fn current() -> io::Result<Self> {
        Ok(Self::new(std::env::current_dir()?))
    }

    /// The directory relative paths start at.
    #[must_use]
    pub fn cwd(&self) -> &Path {
        &self.cwd
    }

    /// The base directory and normalised components of `path`, or `None`
    /// for a path the host cannot name.
    fn split(&self, path: &str) -> Option<(PathBuf, Vec<String>)> {
        let bytes = path.as_bytes();
        let unc = bytes.len() >= 2 && is_separator(bytes[0]) && is_separator(bytes[1]);
        let drive = bytes.len() >= 2 && bytes[1] == b':';
        let (base, rest) = if unc || drive {
            if !cfg!(windows) {
                return None;
            }
            if unc {
                return Some((PathBuf::from(path), Vec::new()));
            }
            if bytes.len() >= 3 && is_separator(bytes[2]) {
                (PathBuf::from(format!("{}\\", &path[..2])), &path[3..])
            } else {
                (PathBuf::from(&path[..2]), &path[2..])
            }
        } else if bytes.first().copied().is_some_and(is_separator) {
            let mut root = PathBuf::new();
            for component in self.cwd.components() {
                match component {
                    Component::Prefix(_) | Component::RootDir => root.push(component),
                    _ => break,
                }
            }
            if root.as_os_str().is_empty() {
                root.push(std::path::MAIN_SEPARATOR_STR);
            }
            (root, &path[1..])
        } else {
            (self.cwd.clone(), path)
        };
        let mut components: Vec<String> = Vec::new();
        for part in rest.split(['\\', '/']) {
            match part {
                "" | "." => {}
                ".." => {
                    components.pop();
                }
                name => components.push(name.to_owned()),
            }
        }
        Some((base, components))
    }

    /// Finds what `path` names: the directory or file `GetFileAttributesA`
    /// would see. `Ok(None)` when nothing is there.
    ///
    /// # Errors
    ///
    /// [`VfsError::NonUtf8Path`] for a path that is not UTF-8.
    pub fn locate(&self, path: &[u8]) -> Result<Option<HostEntry>, VfsError> {
        let path = c_str(path);
        if path.is_empty() {
            return Ok(None);
        }
        let text = core::str::from_utf8(path).map_err(|_| VfsError::NonUtf8Path)?;
        let trailing_separator = path.last().copied().is_some_and(is_separator);
        let Some((base, components)) = self.split(text) else {
            return Ok(None);
        };
        let exact: PathBuf = components.iter().fold(base.clone(), |p, c| p.join(c));
        let found = if fs::metadata(&exact).is_ok() {
            Some(exact)
        } else if cfg!(windows) {
            None
        } else {
            Self::fold_case(base, &components)
        };
        let Some(found) = found else {
            return Ok(None);
        };
        let Ok(meta) = fs::metadata(&found) else {
            return Ok(None);
        };
        if trailing_separator && !meta.is_dir() {
            return Ok(None);
        }
        Ok(Some(HostEntry {
            path: found,
            is_dir: meta.is_dir(),
            readonly: meta.permissions().readonly(),
        }))
    }

    fn fold_case(base: PathBuf, components: &[String]) -> Option<PathBuf> {
        let mut current = base;
        for component in components {
            let direct = current.join(component);
            if fs::symlink_metadata(&direct).is_ok() {
                current = direct;
                continue;
            }
            let mut matches: Vec<String> = fs::read_dir(&current)
                .ok()?
                .filter_map(Result::ok)
                .filter_map(|e| e.file_name().into_string().ok())
                .filter(|name| name.eq_ignore_ascii_case(component))
                .collect();
            matches.sort();
            current = current.join(matches.first()?);
        }
        Some(current)
    }

    /// `GetFileAttributesA(path)`: [`ATTRIBUTE_DIRECTORY`] for a directory,
    /// [`ATTRIBUTE_NORMAL`] for a file, with [`ATTRIBUTE_READONLY`] added
    /// for a read-only one. Other Windows attribute bits are not
    /// reproduced. `Ok(None)` where Windows returns
    /// `INVALID_FILE_ATTRIBUTES`.
    ///
    /// # Errors
    ///
    /// As [`Host::locate`].
    pub fn attributes(&self, path: &[u8]) -> Result<Option<u32>, VfsError> {
        Ok(self.locate(path)?.map(|entry| {
            let kind = if entry.is_dir {
                ATTRIBUTE_DIRECTORY
            } else {
                ATTRIBUTE_NORMAL
            };
            kind | if entry.readonly {
                ATTRIBUTE_READONLY
            } else {
                0
            }
        }))
    }

    /// Opens and reads the file `path` names, as `CreateFileA` with
    /// `OPEN_EXISTING` and a read of the whole mapping would.
    ///
    /// # Errors
    ///
    /// [`VfsError::FileNotFound`] when no file is there, or the host error.
    pub fn read_file(&self, path: &[u8]) -> Result<HostFile, VfsError> {
        let entry = self.locate(path)?.ok_or(VfsError::FileNotFound)?;
        if entry.is_dir {
            return Err(VfsError::AccessDenied);
        }
        let data = fs::read(&entry.path).map_err(|e| host_error(&e))?;
        let meta = fs::metadata(&entry.path).map_err(|e| host_error(&e))?;
        let write = meta.modified().map_or(0, system_time_to_file_time);
        let times = [
            meta.created().map_or(write, system_time_to_file_time),
            meta.accessed().map_or(write, system_time_to_file_time),
            write,
        ];
        Ok(HostFile { data, times })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("neuron-core-host-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("Data").join("Ships")).unwrap();
        fs::write(dir.join("Data").join("Ships").join("Ship.INI"), b"x").unwrap();
        dir
    }

    #[test]
    fn locates_with_either_separator_and_any_case() {
        let dir = scratch("locate");
        let host = Host::new(&dir);
        let found = host.locate(b"data\\SHIPS/ship.ini").unwrap().unwrap();
        assert!(!found.is_dir);
        assert!(host.locate(b"Data\\Ships\\").unwrap().unwrap().is_dir);
        assert_eq!(host.locate(b"Data\\Ships\\Ship.INI\\").unwrap(), None);
        assert!(host.locate(b"Data\\Nope\\..\\Ships").unwrap().is_some());
        assert!(host.locate(b".\\Data\\.\\Ships").unwrap().is_some());
        assert_eq!(host.locate(b"Data\\Missing").unwrap(), None);
        assert_eq!(host.locate(b"").unwrap(), None);
        assert_eq!(host.locate(b"\xff").unwrap_err(), VfsError::NonUtf8Path);
        assert_eq!(host.attributes(b"data").unwrap(), Some(ATTRIBUTE_DIRECTORY));
        assert_eq!(host.read_file(b"DATA\\ships\\SHIP.ini").unwrap().data, b"x");
        assert_eq!(
            host.read_file(b"Data\\none").unwrap_err(),
            VfsError::FileNotFound
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
