//! Path rules of the file systems: `JoinRoot`, the memory file system's
//! `ResolvePath`, the directory-root seal, `CopyClampedPathSuffix` and
//! `_makepath`. Paths are byte strings that end at their first NUL, as the
//! C++ `char*` strings do; both `\` and `/` separate components.

use crate::hash::c_str;

/// The buffer size every internal join uses (`char Resolved[0x105]` with
/// 0x104 passed as the size).
pub const JOIN_CHARS: usize = 0x104;
/// Size of a file system's root-path buffer.
pub const ROOT_PATH_CHARS: usize = 264;

/// `\` or `/`.
#[must_use]
pub const fn is_separator(byte: u8) -> bool {
    byte == b'\\' || byte == b'/'
}

fn at(bytes: &[u8], index: usize) -> u8 {
    bytes.get(index).copied().unwrap_or(0)
}

/// `JoinRoot(root, rel, out, out_chars)` on an output buffer that starts
/// zero-filled, as the callers that read it after a failure have it.
/// Returns the result and the C string the buffer then holds.
///
/// * `out_chars` of 2 or less fails.
/// * A `rel` that starts with a separator, or whose second byte is `:`, is
///   absolute: it is copied, cut to `out_chars - 1` bytes.
/// * Otherwise `root` is copied (failing once `out_chars - 1` bytes are
///   used) and one `\` is added unless it ends in a separator. One leading
///   `.\` of `rel` is dropped, and every leading `..` pops one directory:
///   the first pop steps back from behind the last separator, later pops
///   from on it, and a pop fails when no more than two bytes precede the
///   cursor or no separator is found. The rest of `rel` is appended and
///   the result cut to `out_chars - 1` bytes; with less room than that the
///   join ends at the cursor and may use all `out_chars` bytes.
#[must_use]
pub fn join_root_buffer(root: &[u8], rel: &[u8], out_chars: usize) -> (bool, Vec<u8>) {
    let root = c_str(root);
    let rel = c_str(rel);
    if out_chars <= 2 {
        return (false, Vec::new());
    }
    // One byte more than out_chars: the short-tail case writes its NUL there.
    let mut out = vec![0u8; out_chars + 1];
    let done = |out: &[u8], ok: bool| (ok, c_str(out).to_vec());

    if at(rel, 0) != 0 && (is_separator(at(rel, 0)) || at(rel, 1) == b':') {
        let count = rel.len().min(out_chars);
        out[..count].copy_from_slice(&rel[..count]);
        out[out_chars - 1] = 0;
        return done(&out, true);
    }

    let mut length = 0;
    for &byte in root {
        if length >= out_chars - 1 {
            return done(&out, false);
        }
        out[length] = byte;
        length += 1;
    }

    let mut cursor = length;
    if cursor == 0 || !is_separator(out[cursor - 1]) {
        out[cursor] = b'\\';
        cursor += 1;
    }

    let mut ri = 0;
    if at(rel, 0) == b'.' {
        if is_separator(at(rel, 1)) {
            ri = 2;
        }
        while at(rel, ri) == b'.' && at(rel, ri + 1) == b'.' {
            if cursor <= 2 {
                return done(&out, false);
            }
            cursor -= 2;
            while !is_separator(out[cursor]) {
                if cursor == 0 {
                    return done(&out, false);
                }
                cursor -= 1;
            }
            ri += 2 + usize::from(is_separator(at(rel, ri + 2)));
        }
    }

    if cursor == 0 || !is_separator(out[cursor - 1]) {
        out[cursor] = b'\\';
        cursor += 1;
    }

    let remaining = out_chars - cursor;
    if remaining > 1 {
        let tail = &rel[ri..];
        let count = tail.len().min(remaining);
        out[cursor..cursor + count].copy_from_slice(&tail[..count]);
        out[cursor + count..out_chars].fill(0);
        out[out_chars - 1] = 0;
        return done(&out, true);
    }
    out[cursor] = 0;
    done(&out, true)
}

/// [`join_root_buffer`] for callers that ignore the buffer on failure.
#[must_use]
pub fn join_root(root: &[u8], rel: &[u8], out_chars: usize) -> Option<Vec<u8>> {
    match join_root_buffer(root, rel, out_chars) {
        (true, path) => Some(path),
        (false, _) => None,
    }
}

/// The memory file system's own `ResolvePath(dest, src, dest_chars)`, on a
/// destination that is zero-filled and stays zero past `dest_chars`.
/// Returns the result and the C string the destination then holds.
///
/// Only a leading separator makes `src` absolute. Otherwise the root is
/// copied with `strncpy` (unterminated when it fills the destination),
/// one leading `.\` is dropped, and each leading `..` cuts the last byte
/// and then everything after the last separator; a pop on two bytes or
/// fewer, or on a path without a separator, clears the result but goes on.
/// The rest of `src` is appended with `strncpy`.
#[must_use]
pub fn mem_resolve_path(root: &[u8], src: &[u8], dest_chars: usize) -> (bool, Vec<u8>) {
    let root = c_str(root);
    let src = c_str(src);
    if dest_chars == 0 {
        return (false, Vec::new());
    }
    let mut dest = vec![0u8; dest_chars + 1];
    if is_separator(at(src, 0)) {
        let count = src.len().min(dest_chars);
        dest[..count].copy_from_slice(&src[..count]);
        dest[dest_chars - 1] = 0;
        return (true, c_str(&dest).to_vec());
    }
    let count = root.len().min(dest_chars);
    dest[..count].copy_from_slice(&root[..count]);

    let mut result = true;
    let mut si = 0;
    if at(src, 0) == b'.' {
        if is_separator(at(src, 1)) {
            si = 2;
        }
        while at(src, si) == b'.' && at(src, si + 1) == b'.' {
            let length = c_str(&dest).len();
            if length <= 2 {
                result = false;
            } else {
                dest[length - 1] = 0;
                match dest[..length - 1].iter().rposition(|&b| is_separator(b)) {
                    Some(last) => dest[last + 1] = 0,
                    None => result = false,
                }
            }
            si += 2;
            if is_separator(at(src, si)) {
                si += 1;
            }
        }
    }
    let length = c_str(&dest).len();
    if length < dest_chars {
        let tail = &src[si..];
        let count = tail.len().min(dest_chars - length);
        dest[length..length + count].copy_from_slice(&tail[..count]);
        dest[length + count..dest_chars].fill(0);
    }
    (result, c_str(&dest).to_vec())
}

/// `CopyPath(dest, chars, source)`: at most `chars - 1` bytes.
#[must_use]
pub fn copy_path(source: &[u8], chars: usize) -> Vec<u8> {
    let source = c_str(source);
    source[..source.len().min(chars.saturating_sub(1))].to_vec()
}

/// The directory root a DOS file system adopts (`AdoptDirectoryRoot`):
/// the path cut to 263 bytes, one trailing separator dropped, and `\`
/// written there (at byte 262 at the latest). Returns the root path and
/// the prefix length, the index of that final `\`.
#[must_use]
pub fn seal_directory_root(path: &[u8]) -> (Vec<u8>, usize) {
    let mut root = copy_path(path, ROOT_PATH_CHARS);
    let mut len = root.len();
    if len + 1 >= ROOT_PATH_CHARS {
        len = ROOT_PATH_CHARS - 2;
    }
    if len > 0 && is_separator(root[len - 1]) {
        len -= 1;
    }
    root.truncate(len);
    root.push(b'\\');
    (root, len)
}

/// `CopyClampedPathSuffix(out, max_chars, source)`: the count is the
/// source length raised to 2 and cut to `max_chars`; that many bytes are
/// copied (zeros past the source) and the last one becomes NUL, so a
/// longer source loses its final byte. Returns the C string and the value
/// the C++ returns (the count minus one, or 0 for a zero count).
#[must_use]
pub fn copy_clamped_path_suffix(source: &[u8], max_chars: usize) -> (Vec<u8>, usize) {
    let source = c_str(source);
    let count = source.len().max(2).min(max_chars);
    if count == 0 {
        return (Vec::new(), 0);
    }
    let kept = source.len().min(count - 1);
    (source[..kept].to_vec(), count - 1)
}

/// `_makepath(out, NULL, dir, fname, NULL)`: `dir`, a `\` unless it is
/// empty or ends in a separator, then `fname` as it is.
#[must_use]
pub fn make_path(dir: &[u8], fname: &[u8]) -> Vec<u8> {
    let dir = c_str(dir);
    let mut out = dir.to_vec();
    if let Some(&last) = dir.last()
        && !is_separator(last)
    {
        out.push(b'\\');
    }
    out.extend_from_slice(c_str(fname));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn join(root: &str, rel: &str) -> Option<String> {
        join_root(root.as_bytes(), rel.as_bytes(), JOIN_CHARS)
            .map(|p| String::from_utf8(p).unwrap())
    }

    #[test]
    fn joins() {
        assert_eq!(join("\\", "Mesh").as_deref(), Some("\\Mesh"));
        assert_eq!(join("data", "x.txt").as_deref(), Some("data\\x.txt"));
        assert_eq!(join("data\\", "x.txt").as_deref(), Some("data\\x.txt"));
        assert_eq!(join("", "x").as_deref(), Some("\\x"));
        assert_eq!(join("a", "\\abs").as_deref(), Some("\\abs"));
        assert_eq!(join("a", "C:\\abs").as_deref(), Some("C:\\abs"));
        assert_eq!(join("a", "9:x").as_deref(), Some("9:x"));
        assert_eq!(join("\\a\\", ".\\b").as_deref(), Some("\\a\\b"));
        assert_eq!(join("\\Animation\\", "..").as_deref(), Some("\\"));
        assert_eq!(join("\\ab\\cd\\", "..\\x").as_deref(), Some("\\ab\\x"));
        assert_eq!(join("\\ab\\cd\\", "..\\..\\x").as_deref(), Some("\\x"));
        // The second pop starts on a separator: one-byte names cannot pop twice.
        assert_eq!(join("\\a\\b\\", "..\\..\\x"), None);
        assert_eq!(join("C:\\", ".."), None);
        assert_eq!(join("data", ".."), None);
        assert_eq!(join("\\", ".."), None);
        // Only leading dots are special.
        assert_eq!(join("a", "b\\..\\c").as_deref(), Some("a\\b\\..\\c"));
    }

    #[test]
    fn join_limits() {
        assert_eq!(join_root_buffer(b"a", b"b", 2), (false, Vec::new()));
        assert_eq!(join_root(b"ab", b"cdef", 6), Some(b"ab\\cd".to_vec()));
        // The root fills the buffer: the separator lands in the last byte
        // and the NUL one past it.
        assert_eq!(join_root(b"abcde", b"x", 6), Some(b"abcde\\".to_vec()));
        assert_eq!(join_root(b"abcd", b"x", 6), Some(b"abcd\\".to_vec()));
        assert_eq!(
            join_root_buffer(b"abcdef", b"x", 6),
            (false, b"abcde".to_vec())
        );
        assert_eq!(join_root(b"x", b"\\abcdef", 4), Some(b"\\ab".to_vec()));
    }

    #[test]
    fn mem_resolves() {
        let r = |root: &str, src: &str, n| {
            let (ok, p) = mem_resolve_path(root.as_bytes(), src.as_bytes(), n);
            (ok, String::from_utf8(p).unwrap())
        };
        assert_eq!(r("dir\\", "x", 64), (true, "dir\\x".into()));
        assert_eq!(r("dir", "x", 64), (true, "dirx".into()));
        assert_eq!(r("a\\b\\", "..\\x", 64), (true, "a\\x".into()));
        assert_eq!(r("ab", "..\\x", 64), (false, "abx".into()));
        assert_eq!(r("abc", "..\\x", 64), (false, "abx".into()));
        assert_eq!(r("r", "/abs", 3), (true, "/a".into()));
        assert_eq!(r("root", "x", 3), (true, "roo".into()));
    }

    #[test]
    fn seals_and_suffixes() {
        assert_eq!(seal_directory_root(b"data"), (b"data\\".to_vec(), 4));
        assert_eq!(seal_directory_root(b"data/"), (b"data\\".to_vec(), 4));
        assert_eq!(seal_directory_root(b""), (b"\\".to_vec(), 0));
        let long = vec![b'x'; 300];
        let (root, prefix) = seal_directory_root(&long);
        assert_eq!((root.len(), prefix), (263, 262));
        assert_eq!(
            copy_clamped_path_suffix(b"\\sub\\", 64),
            (b"\\sub".to_vec(), 4)
        );
        assert_eq!(copy_clamped_path_suffix(b"\\", 64), (b"\\".to_vec(), 1));
        assert_eq!(copy_clamped_path_suffix(b"ab", 64), (b"a".to_vec(), 1));
        assert_eq!(copy_clamped_path_suffix(b"", 64), (Vec::new(), 1));
        assert_eq!(copy_clamped_path_suffix(b"abcdef", 3), (b"ab".to_vec(), 2));
        assert_eq!(copy_clamped_path_suffix(b"abcdef", 0), (Vec::new(), 0));
        assert_eq!(make_path(b"dir", b"x.utf"), b"dir\\x.utf");
        assert_eq!(make_path(b"dir/", b"x.utf"), b"dir/x.utf");
        assert_eq!(make_path(b"", b"x.utf"), b"x.utf");
    }
}
