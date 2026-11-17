//! The search path (`FSearchPathInstance`): an ordered list of roots, each
//! a DOS file system, asked in turn.

use super::path::is_separator;
use super::{FileSystem, FsDesc, Host, VfsError, create_file_system};
use crate::hash::c_str;

/// Bytes of the path list a search path keeps (`strncpy(..., 0x104)`).
pub const SEARCH_PATH_CHARS: usize = 0x104;

fn at(bytes: &[u8], index: usize) -> u8 {
    bytes.get(index).copied().unwrap_or(0)
}

/// `SplitSearchPath`: the list cut to [`SEARCH_PATH_CHARS`] bytes, split
/// at `;`, empty entries dropped; the roots keep the list's order.
#[must_use]
pub fn split_search_path(list: &[u8]) -> Vec<&[u8]> {
    let list = c_str(list);
    let list = &list[..list.len().min(SEARCH_PATH_CHARS)];
    list.split(|&b| b == b';')
        .filter(|s| !s.is_empty())
        .collect()
}

/// The split `OpenSearchPathEntry` makes: the first component it opens and
/// the rest it opens inside it, or `None` where it gives up.
///
/// * A UNC path `\\server\share\rest` keeps `\\server\share` together; a
///   third leading separator, a missing share or a separator right after
///   the server's gives up.
/// * A drive path `X:\dir\rest` keeps `X:\dir` together.
/// * Otherwise the first component runs from the first byte (a leading
///   separator included) to the next separator.
///
/// The reference reads past the end of an entry in a few malformed cases
/// (`\\` alone, `X:` alone); here the bytes past the end read as NUL
/// (divergence register).
#[must_use]
pub fn split_search_entry(path: &[u8]) -> Option<(&[u8], Option<&[u8]>)> {
    let p = c_str(path);
    let split = |c: usize| {
        if at(p, c) != 0 {
            (&p[..c], Some(&p[c + 1..]))
        } else {
            (p, None)
        }
    };
    if is_separator(at(p, 0)) && is_separator(at(p, 1)) {
        if is_separator(at(p, 2)) {
            return None;
        }
        let mut c = 3;
        if at(p, c) == 0 {
            return None;
        }
        while !is_separator(at(p, c)) {
            c += 1;
            if at(p, c) == 0 {
                return None;
            }
        }
        c += 1;
        if is_separator(at(p, c)) {
            return None;
        }
        while at(p, c) != 0 && !is_separator(at(p, c)) {
            c += 1;
        }
        return Some(split(c));
    }
    let mut c = 1;
    if at(p, 0).is_ascii_alphabetic() && at(p, 1) == b':' {
        c = 3;
    }
    while at(p, c) != 0 && !is_separator(at(p, c)) {
        c += 1;
    }
    Some(split(c))
}

/// `OpenSearchPathEntry`: the first component through the component
/// manager as a `"DOS"` file system, then each further component inside the
/// one before.
fn open_entry(
    host: &Host,
    parent: Option<&dyn FileSystem>,
    path: &[u8],
) -> Option<Box<dyn FileSystem>> {
    let (first, rest) = split_search_entry(path)?;
    let desc = FsDesc::read(first).with_implementation(b"DOS");
    let created = match parent {
        None => create_file_system(host, &desc),
        Some(parent) => parent.create_child(&desc),
    }
    .ok()?;
    match rest {
        Some(rest) if !rest.is_empty() => open_entry(host, Some(created.as_ref()), rest),
        _ => Some(created),
    }
}

/// An ordered list of roots. A root that could not be opened keeps its
/// place as `None`.
#[derive(Debug, Default)]
pub struct SearchPath {
    text: Vec<u8>,
    roots: Vec<Option<Box<dyn FileSystem>>>,
}

impl SearchPath {
    /// An empty search path.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `SetPath(list)`: replaces the roots with the entries of
    /// [`split_search_path`], each opened as [`split_search_entry`]
    /// describes. Relative entries start at the host's current directory.
    pub fn set_path(&mut self, host: &Host, list: &[u8]) {
        let list = c_str(list);
        self.text = list[..list.len().min(SEARCH_PATH_CHARS)].to_vec();
        self.roots = split_search_path(&self.text)
            .into_iter()
            .map(|entry| open_entry(host, None, entry))
            .collect();
    }

    /// The path list kept, at most [`SEARCH_PATH_CHARS`] bytes.
    #[must_use]
    pub fn path(&self) -> &[u8] {
        &self.text
    }

    /// `GetPath(dest, max_chars)`: the count the C++ returns (the length
    /// plus one, cut to `max_chars`) and the bytes before the NUL it writes
    /// at that count.
    #[must_use]
    pub fn get_path(&self, max_chars: usize) -> (usize, Vec<u8>) {
        let count = (self.text.len() + 1).min(max_chars);
        if count == 0 {
            return (0, Vec::new());
        }
        (count, self.text[..count - 1].to_vec())
    }

    /// The roots in order.
    #[must_use]
    pub fn roots(&self) -> &[Option<Box<dyn FileSystem>>] {
        &self.roots
    }

    /// `CreateInstance`: asks each opened root in order and returns the
    /// first success.
    ///
    /// # Errors
    ///
    /// The last root's error, or [`VfsError::OpenFailed`] when no root is
    /// open. A file the C++ would wrap in a broken file system
    /// ([`VfsError::NotAnArchive`]) ends the search, as the C++'s
    /// "success" does.
    pub fn create(&self, desc: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
        let mut result = Err(VfsError::OpenFailed);
        for root in self.roots.iter().flatten() {
            match root.create_child(desc) {
                Ok(fs) => return Ok(fs),
                Err(VfsError::NotAnArchive) => return Err(VfsError::NotAnArchive),
                Err(e) => result = Err(e),
            }
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_lists() {
        assert_eq!(split_search_path(b"a;b;c"), [&b"a"[..], b"b", b"c"]);
        assert_eq!(split_search_path(b";;a;;;b;"), [&b"a"[..], b"b"]);
        assert!(split_search_path(b"").is_empty());
        let long = [b'x'; 300];
        assert_eq!(split_search_path(&long)[0].len(), SEARCH_PATH_CHARS);
    }

    #[test]
    fn splits_entries() {
        assert_eq!(
            split_search_entry(b"data\\ships"),
            Some((&b"data"[..], Some(&b"ships"[..])))
        );
        assert_eq!(split_search_entry(b"data"), Some((&b"data"[..], None)));
        assert_eq!(
            split_search_entry(b"data\\"),
            Some((&b"data"[..], Some(&b""[..])))
        );
        assert_eq!(
            split_search_entry(b"\\root\\x"),
            Some((&b"\\root"[..], Some(&b"x"[..])))
        );
        assert_eq!(
            split_search_entry(b"C:\\games\\data"),
            Some((&b"C:\\games"[..], Some(&b"data"[..])))
        );
        assert_eq!(
            split_search_entry(b"\\\\srv\\share\\dir"),
            Some((&b"\\\\srv\\share"[..], Some(&b"dir"[..])))
        );
        assert_eq!(
            split_search_entry(b"\\\\srv\\share"),
            Some((&b"\\\\srv\\share"[..], None))
        );
        assert_eq!(split_search_entry(b"\\\\\\x"), None);
        assert_eq!(split_search_entry(b"\\\\srv"), None);
        assert_eq!(split_search_entry(b"\\\\srv\\\\share"), None);
        assert_eq!(
            split_search_entry(b"1:\\x"),
            Some((&b"1:"[..], Some(&b"x"[..])))
        );
    }
}
