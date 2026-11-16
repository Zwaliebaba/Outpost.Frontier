//! The `*` and `?` matcher of the UTF find functions (`MatchUtfWildcard`
//! and `MatchUtfStarQuestion` in `CppSource/ReadFile/ReadFile.cpp`).

use std::collections::HashMap;

use crate::hash::c_str;

/// Size of the two stack buffers `MatchUtfWildcard` prepares.
const BUFFER: usize = 264;

/// The 128 bytes `FoldUtfWildChar` reads for a byte at or above 0x80.
///
/// The original folds with a signed `char` index into a lower-case table,
/// so bytes at or above 0x80 read the unrelated read-only data in front of
/// it: the tail of a source path and a message string. The reconstruction
/// keeps those bytes as `G_UtfFoldBelowTable`.
pub const FOLD_BELOW_TABLE: [u8; 128] = [
    0x62, 0x75, 0x69, 0x6C, 0x64, 0x73, 0x5C, 0x64, 0x61, 0x6C, 0x69, 0x62, 0x73, 0x5C, 0x64, 0x61,
    0x6C, 0x69, 0x62, 0x73, 0x2D, 0x62, 0x75, 0x69, 0x6C, 0x64, 0x5C, 0x62, 0x75, 0x69, 0x6C, 0x64,
    0x5C, 0x53, 0x72, 0x63, 0x5C, 0x44, 0x4F, 0x53, 0x46, 0x69, 0x6C, 0x65, 0x5C, 0x52, 0x65, 0x61,
    0x64, 0x46, 0x69, 0x6C, 0x65, 0x5C, 0x42, 0x61, 0x73, 0x65, 0x55, 0x54, 0x46, 0x2E, 0x63, 0x70,
    0x70, 0x00, 0x00, 0x00, 0x61, 0x74, 0x74, 0x65, 0x6D, 0x70, 0x74, 0x69, 0x6E, 0x67, 0x20, 0x74,
    0x6F, 0x20, 0x63, 0x6C, 0x6F, 0x73, 0x65, 0x20, 0x69, 0x6E, 0x76, 0x61, 0x6C, 0x69, 0x64, 0x20,
    0x68, 0x61, 0x6E, 0x64, 0x6C, 0x65, 0x20, 0x28, 0x25, 0x73, 0x29, 0x00, 0x31, 0x2E, 0x31, 0x31,
    0x5F, 0x49, 0x46, 0x69, 0x6C, 0x65, 0x53, 0x79, 0x73, 0x74, 0x65, 0x6D, 0x00, 0x00, 0x00, 0x00,
];

/// `FoldUtfWildChar`: `A`..=`Z` to lower case, `\` to `/`, and a byte at
/// or above 0x80 through [`FOLD_BELOW_TABLE`].
#[must_use]
pub const fn fold_wild_char(byte: u8) -> u8 {
    if byte >= 0x80 {
        FOLD_BELOW_TABLE[(byte - 0x80) as usize]
    } else if byte.is_ascii_uppercase() {
        byte + (b'a' - b'A')
    } else if byte == b'\\' {
        b'/'
    } else {
        byte
    }
}

/// `CopyPath`: `strncpy` of at most `dest.len() - 1` bytes, zero padding,
/// and a NUL in the last byte.
fn copy_path(dest: &mut [u8], source: &[u8]) {
    dest.fill(0);
    let count = source.len().min(dest.len().saturating_sub(1));
    dest[..count].copy_from_slice(&source[..count]);
}

fn strlen(buffer: &[u8]) -> usize {
    c_str(buffer).len()
}

/// `MatchUtfWildcard(name, pattern)`, the test `FindNext` applies to each
/// record name.
///
/// A pattern without a dot gets `.*` appended, a pattern that starts with
/// a dot gets `*` in front, and a name without a dot gets a dot appended,
/// each within 264-byte buffers. The matcher then compares byte by byte
/// through [`fold_wild_char`]. Both inputs end at their first NUL byte.
///
/// The reference reads past the end of a buffer when a pattern or name
/// byte that folds to 0 meets the other string's terminator; past the 264
/// bytes it reads unrelated stack memory. Here every byte past a buffer
/// reads as 0 (divergence register).
#[must_use]
pub fn match_wildcard(name: &[u8], pattern: &[u8]) -> bool {
    let name = c_str(name);
    let pattern = c_str(pattern);

    let mut prepared_pattern = [0u8; BUFFER];
    match pattern.iter().position(|&b| b == b'.') {
        None => {
            copy_path(&mut prepared_pattern, pattern);
            let len = strlen(&prepared_pattern);
            if len + 2 < BUFFER {
                prepared_pattern[len] = b'.';
                prepared_pattern[len + 1] = b'*';
                prepared_pattern[len + 2] = 0;
            }
        }
        Some(0) => {
            prepared_pattern[0] = b'*';
            copy_path(&mut prepared_pattern[1..], pattern);
        }
        Some(_) => copy_path(&mut prepared_pattern, pattern),
    }

    let mut prepared_name = [0u8; BUFFER];
    copy_path(&mut prepared_name, name);
    if !name.contains(&b'.') {
        let len = strlen(&prepared_name);
        if len + 1 < BUFFER {
            prepared_name[len] = b'.';
            prepared_name[len + 1] = 0;
        }
    }

    let mut memo = HashMap::new();
    star_question(&prepared_name, &prepared_pattern, 0, 0, &mut memo)
}

fn at(buffer: &[u8], index: usize) -> u8 {
    buffer.get(index).copied().unwrap_or(0)
}

/// `MatchUtfStarQuestion` over the prepared buffers. The result for a `*`
/// at a given pair of positions is remembered, which keeps the recursion
/// polynomial without changing any answer.
fn star_question(
    name: &[u8],
    pattern: &[u8],
    mut ni: usize,
    mut pi: usize,
    memo: &mut HashMap<(usize, usize), bool>,
) -> bool {
    loop {
        let pc = at(pattern, pi);
        if pc == b'*' {
            if let Some(&known) = memo.get(&(ni, pi)) {
                return known;
            }
            let start = ni;
            let mut matched = star_question(name, pattern, ni, pi + 1, memo);
            if !matched && at(name, ni) != 0 {
                loop {
                    ni += 1;
                    matched = star_question(name, pattern, ni, pi + 1, memo);
                    if matched || at(name, ni) == 0 {
                        break;
                    }
                }
            }
            memo.insert((start, pi), matched);
            return matched;
        }
        let nc = at(name, ni);
        if pc == 0 {
            if nc == 0 {
                return true;
            }
            if fold_wild_char(0) != fold_wild_char(nc) {
                return false;
            }
        } else if pc != b'?' {
            if fold_wild_char(pc) != fold_wild_char(nc) {
                return false;
            }
        } else if nc == 0 {
            return false;
        }
        pi += 1;
        ni += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn folds() {
        assert_eq!(fold_wild_char(b'A'), b'a');
        assert_eq!(fold_wild_char(b'z'), b'z');
        assert_eq!(fold_wild_char(b'\\'), b'/');
        assert_eq!(fold_wild_char(b'/'), b'/');
        assert_eq!(fold_wild_char(0x80), b'b');
        assert_eq!(fold_wild_char(0xC1), 0);
        assert_eq!(fold_wild_char(0xFF), 0);
    }

    #[test]
    fn basic_patterns() {
        assert!(match_wildcard(b"Geometry", b"*"));
        assert!(match_wildcard(b"Geometry", b"geo*"));
        assert!(match_wildcard(b"MIP0", b"MIP*"));
        assert!(!match_wildcard(b"Texture", b"MIP*"));
        assert!(match_wildcard(b"a.txt", b"*.TXT"));
        assert!(match_wildcard(b"a.txt", b".txt"));
        assert!(match_wildcard(b"abc", b"a?c"));
        assert!(!match_wildcard(b"ac", b"a?c"));
        assert!(match_wildcard(b"noext", b"*."));
        assert!(!match_wildcard(b"x.y", b"*."));
    }

    #[test]
    fn high_bytes_fold_through_the_table() {
        // 0x80 folds to 'b' (0x62), so it matches 'b' and 'B'.
        assert!(match_wildcard(b"\x80", b"B"));
        // 0xC1 folds to 0 and so matches the pattern's end.
        assert!(match_wildcard(b"a.\xC1", b"a.*"));
        assert!(!match_wildcard(b"a\xC1\xC1", b"a"));
        // A pattern byte that folds to 0 runs past the name's terminator.
        assert!(match_wildcard(b"ab.c", b"ab.c\xC1"));
    }

    #[test]
    fn many_stars_stay_fast() {
        let pattern = [b'*'; 200];
        let name = [b'x'; 250];
        assert!(match_wildcard(&name, &pattern));
        let mut pattern = b"*a".repeat(100);
        pattern.push(b'b');
        assert!(!match_wildcard(&[b'a'; 250], &pattern));
    }
}
