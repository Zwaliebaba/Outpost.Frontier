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
//!
//! [`Host::find`] stands for `FindFirstFileA` / `FindNextFileA`: the last
//! component is a pattern matched with the Win32 wildcard rules
//! ([`win32_match`]) against the directory's entries, `.` and `..`
//! included. Entries come in byte order of their names, not in the order
//! of the host's file system (divergence D37), and short (8.3) names are
//! not matched.

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

/// One entry [`Host::find`] returns, as `WIN32_FIND_DATAA` describes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostFindEntry {
    /// The name, `.` and `..` included.
    pub name: Vec<u8>,
    /// Whether it is a directory.
    pub is_dir: bool,
    /// Whether it is read-only.
    pub readonly: bool,
    /// The size; 0 for a directory.
    pub size: u64,
    /// Creation, last access and last write times as `FILETIME` counts.
    pub times: [u64; 3],
}

/// One element of a pattern after the Win32 layer has translated it for
/// `FsRtlIsNameInExpression`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Wild {
    /// `*` not followed by `.`: any run of characters.
    Star,
    /// `*` followed by `.` (`DOS_STAR`): any run that does not take the
    /// name's last `.`.
    DosStar,
    /// `?` (`DOS_QM`): one character, or nothing at a `.` or at the end.
    DosQm,
    /// `.` followed by `?`, `*` or the end (`DOS_DOT`): a `.`, or nothing
    /// at the end of the name.
    DosDot,
    /// Any other byte, compared without ASCII case.
    Byte(u8),
}

fn translate(pattern: &[u8]) -> Vec<Wild> {
    pattern
        .iter()
        .enumerate()
        .map(|(i, &b)| {
            let next = pattern.get(i + 1).copied();
            match b {
                b'*' if next == Some(b'.') => Wild::DosStar,
                b'*' => Wild::Star,
                b'?' => Wild::DosQm,
                b'.' if matches!(next, None | Some(b'?' | b'*')) => Wild::DosDot,
                other => Wild::Byte(other),
            }
        })
        .collect()
}

/// Whether `name` matches `pattern` as `FindFirstFileA` matches it: the
/// pattern translated as the Win32 layer does (`?` to `DOS_QM`, `*` before
/// a `.` to `DOS_STAR`, a `.` before `?`, `*` or the end to `DOS_DOT`),
/// then matched as `FsRtlIsNameInExpression` matches, without ASCII case.
/// So `*.*` matches every name, `*.` the names without an extension, and
/// `Ship?.utf` both `Ship.utf` and `Ships.utf`.
///
/// For the `.` and `..` entries a `?` takes a `.` like any other
/// character, so `????` lists them: the oracle shows Windows doing so on
/// NTFS, while `?ub` still does not list them.
#[must_use]
pub fn win32_match(pattern: &[u8], name: &[u8]) -> bool {
    let pattern = translate(pattern);
    let dot_entry = !name.is_empty() && name.iter().all(|&b| b == b'.');
    let last_dot = name.iter().rposition(|&b| b == b'.');
    // reachable[n]: the pattern prefix so far can end at name position n.
    let mut reachable = vec![false; name.len() + 1];
    reachable[0] = true;
    for wild in pattern {
        let mut next = vec![false; name.len() + 1];
        for n in 0..=name.len() {
            if !reachable[n] {
                continue;
            }
            match wild {
                Wild::Star => next[n..].iter_mut().for_each(|r| *r = true),
                Wild::DosStar => {
                    next[n] = true;
                    let mut end = n;
                    while end < name.len() && Some(end) != last_dot {
                        end += 1;
                        next[end] = true;
                    }
                }
                Wild::DosQm => {
                    if n == name.len() || (name[n] == b'.' && !dot_entry) {
                        next[n] = true;
                    } else {
                        next[n + 1] = true;
                    }
                }
                Wild::DosDot => {
                    if n == name.len() {
                        next[n] = true;
                    } else if name[n] == b'.' {
                        next[n + 1] = true;
                    }
                }
                Wild::Byte(b) => {
                    if n < name.len() && name[n].eq_ignore_ascii_case(&b) {
                        next[n + 1] = true;
                    }
                }
            }
        }
        reachable = next;
    }
    reachable[name.len()]
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
        Ok(HostFile {
            data,
            times: times_of(&meta),
        })
    }

    /// `FindFirstFileA(path)` and the `FindNextFileA` calls after it: the
    /// entries of the directory `path` names up to its last separator whose
    /// names match the last component ([`win32_match`]), in byte order of
    /// their names. A directory that is not a volume root also lists `.`
    /// and `..`.
    ///
    /// # Errors
    ///
    /// [`VfsError::PathNotFound`] when the directory part names no
    /// directory, [`VfsError::FileNotFound`] when nothing matches or the
    /// pattern is empty, [`VfsError::NonUtf8Path`] for a path that is not
    /// UTF-8, or the host error.
    pub fn find(&self, path: &[u8]) -> Result<Vec<HostFindEntry>, VfsError> {
        let path = c_str(path);
        if core::str::from_utf8(path).is_err() {
            return Err(VfsError::NonUtf8Path);
        }
        let split = path.iter().rposition(|&b| is_separator(b) || b == b':');
        let (directory, pattern) = match split {
            Some(at) => (&path[..=at], &path[at + 1..]),
            None => (&b""[..], path),
        };
        if pattern.is_empty() {
            return Err(VfsError::FileNotFound);
        }
        let directory_path = if directory.is_empty() {
            self.cwd.clone()
        } else {
            match self.locate(directory)? {
                Some(entry) if entry.is_dir => entry.path,
                _ => return Err(VfsError::PathNotFound),
            }
        };
        let meta = fs::metadata(&directory_path).map_err(|e| host_error(&e))?;
        let mut entries = Vec::new();
        if directory_path.parent().is_some() {
            for name in [&b"."[..], b".."] {
                entries.push(HostFindEntry {
                    name: name.to_vec(),
                    is_dir: true,
                    readonly: false,
                    size: 0,
                    times: times_of(&meta),
                });
            }
        }
        for item in fs::read_dir(&directory_path).map_err(|e| host_error(&e))? {
            let item = item.map_err(|e| host_error(&e))?;
            let Ok(name) = item.file_name().into_string() else {
                continue;
            };
            let Ok(meta) = fs::metadata(item.path()) else {
                continue;
            };
            entries.push(HostFindEntry {
                name: name.into_bytes(),
                is_dir: meta.is_dir(),
                readonly: meta.permissions().readonly(),
                size: if meta.is_dir() { 0 } else { meta.len() },
                times: times_of(&meta),
            });
        }
        entries.retain(|entry| win32_match(pattern, &entry.name));
        if entries.is_empty() {
            return Err(VfsError::FileNotFound);
        }
        entries.sort_by(|a, b| a.name.cmp(&b.name));
        Ok(entries)
    }
}

/// Creation, last access and last write times; a time the host does not
/// keep is the last write time.
fn times_of(meta: &fs::Metadata) -> [u64; 3] {
    let write = meta.modified().map_or(0, system_time_to_file_time);
    [
        meta.created().map_or(write, system_time_to_file_time),
        meta.accessed().map_or(write, system_time_to_file_time),
        write,
    ]
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

    #[test]
    fn win32_wildcards() {
        let all = |p: &[u8]| {
            [
                &b"."[..],
                b"..",
                b"Deep",
                b"plain.cmp",
                b"Ship.utf",
                b"Ships.utf",
                b"a.b.c",
            ]
            .iter()
            .filter(|n| win32_match(p, n))
            .map(|n| String::from_utf8_lossy(n).into_owned())
            .collect::<Vec<_>>()
        };
        let names = |v: &[&str]| v.iter().map(|s| (*s).to_owned()).collect::<Vec<_>>();
        let every = names(&[
            ".",
            "..",
            "Deep",
            "plain.cmp",
            "Ship.utf",
            "Ships.utf",
            "a.b.c",
        ]);
        assert_eq!(all(b"*"), every);
        assert_eq!(all(b"*.*"), every);
        assert_eq!(all(b"*."), names(&[".", "..", "Deep"]));
        assert_eq!(all(b"Ship?.utf"), names(&["Ship.utf", "Ships.utf"]));
        assert_eq!(all(b"SHIP.UTF"), names(&["Ship.utf"]));
        assert_eq!(all(b"*.c"), names(&["a.b.c"]));
        assert_eq!(all(b"*.b"), Vec::<String>::new());
        // DOS_QM takes no '.' of an ordinary name, but does take the dots of
        // '.' and '..' (Windows lists both for '????').
        assert_eq!(all(b"?"), names(&["."]));
        assert_eq!(all(b"??"), names(&[".", ".."]));
        assert_eq!(all(b"????"), names(&[".", "..", "Deep"]));
        assert_eq!(all(b"?.utf"), Vec::<String>::new());
        assert_eq!(all(b"p*"), names(&["plain.cmp"]));
        assert_eq!(all(b"*s*"), names(&["Ship.utf", "Ships.utf"]));
    }

    #[test]
    fn finds_like_find_first_file() {
        let dir = scratch("find");
        fs::write(dir.join("Data").join("note.txt"), b"abc").unwrap();
        let host = Host::new(&dir);
        let names = |p: &[u8]| -> Vec<Vec<u8>> {
            host.find(p).unwrap().into_iter().map(|e| e.name).collect()
        };
        assert_eq!(
            names(b"data\\*"),
            vec![
                b".".to_vec(),
                b"..".to_vec(),
                b"Ships".to_vec(),
                b"note.txt".to_vec()
            ]
        );
        let note = &host.find(b"Data\\NOTE.*").unwrap()[0];
        assert_eq!((note.size, note.is_dir), (3, false));
        assert!(host.find(b"Data\\Ships").unwrap()[0].is_dir);
        assert_eq!(
            host.find(b"Data\\*.zip").unwrap_err(),
            VfsError::FileNotFound
        );
        assert_eq!(host.find(b"Nope\\*").unwrap_err(), VfsError::PathNotFound);
        assert_eq!(host.find(b"Data\\").unwrap_err(), VfsError::FileNotFound);
        let _ = fs::remove_dir_all(&dir);
    }
}
