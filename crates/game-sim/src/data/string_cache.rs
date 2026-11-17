//! The string cache (`StringAlloc`, `CacheString`), ported from
//! `CppSource/Common/common_stringcache.cpp`.
//!
//! The C++ copies every string into one static pool of 0x18_0000 bytes and
//! hands out pointers into it. Interned strings are looked up first in an
//! index ordered by `stricmp`, so interning is case-insensitive: the first
//! spelling of a name wins and every later request with other letter case
//! gets that first copy back. Temporary strings are copied each time and
//! never looked up. Nothing is freed or moved.
//!
//! The port keeps every observable part: the first-spelling rule, pointer
//! identity (`EquipDesc::is_internal` compares pointers), the pool budget
//! (both cursors, so an overflow happens at the same request and yields the
//! shared empty string), and writes through a pointer: the docking-sphere
//! reader lower-cases an interned copy in place, which every holder of that
//! copy sees. A [`CacheString`] is therefore a handle into the
//! [`StringCache`], which holds the bytes.

use std::collections::HashMap;

/// Bytes in the C++ pool (`StringCachePoolBytes`).
pub const POOL_BYTES: u32 = 0x18_0000;

/// A string handed out by the cache (`CacheString`, a pointer into the
/// pool). The default is the shared empty string; every other handle names
/// a non-empty string. [`StringCache::get`] reads it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CacheString(u32);

impl CacheString {
    /// The shared empty string (pool byte 0).
    pub const EMPTY: Self = Self(0);

    /// Whether this is the empty string (`Value[0] == '\0'`).
    #[must_use]
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `CacheString::clear`: the shared empty string.
    pub fn clear(&mut self) {
        *self = Self::EMPTY;
    }
}

/// Something `StringAlloc` reports through `FDUMP`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StringCacheReport {
    /// `StringCache overflow! (1536 KB)`: the request did not fit; the
    /// shared empty string was returned. `temporary` tells which of the two
    /// report sites (line 0xb9 for temporary, 0xa9 for interned) it was.
    Overflow {
        /// The request was for a temporary copy.
        temporary: bool,
    },
}

/// The pool, its two cursors and the intern index.
#[derive(Clone, Debug)]
pub struct StringCache {
    entries: Vec<Vec<u8>>,
    bottom: u32,
    top: u32,
    interned: HashMap<Vec<u8>, CacheString>,
    interned_count: u32,
    temporary_count: u32,
    reports: Vec<StringCacheReport>,
    cargo_bay: CacheString,
}

impl Default for StringCache {
    /// The cache after the static initialisers: `EquipDesc::CARGO_BAY_HP_NAME`
    /// (`StringAlloc("BAY", false)`) is the first string interned.
    fn default() -> Self {
        let mut cache = Self {
            entries: vec![Vec::new()],
            bottom: 1,
            top: POOL_BYTES,
            interned: HashMap::new(),
            interned_count: 1,
            temporary_count: 0,
            reports: Vec::new(),
            cargo_bay: CacheString::EMPTY,
        };
        cache.cargo_bay = cache.alloc(b"BAY", false);
        cache
    }
}

/// The intern key: equal keys are exactly the names `stricmp` calls equal.
fn fold(text: &[u8]) -> Vec<u8> {
    text.iter().map(u8::to_ascii_lowercase).collect()
}

impl StringCache {
    /// An empty cache, as after the static initialisers.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The bytes of `string`, without a terminator.
    #[must_use]
    pub fn get(&self, string: CacheString) -> &[u8] {
        self.entries
            .get(string.0 as usize)
            .map_or(&[][..], Vec::as_slice)
    }

    /// The bytes of `string` as text, invalid UTF-8 replaced (messages).
    #[must_use]
    pub fn lossy(&self, string: CacheString) -> String {
        String::from_utf8_lossy(self.get(string)).into_owned()
    }

    /// `StringAlloc(text, temporary)`. `text` ends at its first NUL byte.
    pub fn alloc(&mut self, text: &[u8], temporary: bool) -> CacheString {
        let text = neuron_core::hash::c_str(text);
        if text.is_empty() {
            return CacheString::EMPTY;
        }
        let length = u32::try_from(text.len()).unwrap_or(u32::MAX);
        if temporary {
            if length.saturating_add(1) > self.top - self.bottom {
                self.reports
                    .push(StringCacheReport::Overflow { temporary: true });
                return CacheString::EMPTY;
            }
            self.top -= length + 1;
            self.temporary_count += 1;
            return self.push(text);
        }
        let key = fold(text);
        if let Some(&found) = self.interned.get(&key) {
            return found;
        }
        if length.saturating_add(1) > self.top - self.bottom {
            self.reports
                .push(StringCacheReport::Overflow { temporary: false });
            return CacheString::EMPTY;
        }
        let copy = self.push(text);
        self.interned.insert(key, copy);
        self.interned_count += 1;
        self.bottom += length + 1;
        copy
    }

    fn push(&mut self, text: &[u8]) -> CacheString {
        let handle = CacheString(u32::try_from(self.entries.len()).unwrap_or(u32::MAX));
        self.entries.push(text.to_vec());
        handle
    }

    /// `_strlwr` on a pooled copy: every holder of `string` sees the lower
    /// case text. The index stays valid, since it compares without case.
    pub fn lowercase_in_place(&mut self, string: CacheString) {
        if let Some(entry) = self.entries.get_mut(string.0 as usize) {
            entry.make_ascii_lowercase();
        }
    }

    /// `EquipDesc::CARGO_BAY_HP_NAME`: the internal cargo bay's hardpoint.
    /// An equipment line is internal exactly when its hardpoint is this
    /// copy.
    #[must_use]
    pub fn cargo_bay(&self) -> CacheString {
        self.cargo_bay
    }

    /// `StringShutdown`: drops the index. The cursors stay, so later
    /// requests keep using up the pool, and a name interned before is
    /// copied again.
    pub fn shutdown(&mut self) {
        self.interned.clear();
    }

    /// Bytes still free between the two cursors.
    #[must_use]
    pub fn free_bytes(&self) -> u32 {
        self.top - self.bottom
    }

    /// The interned strings counted so far, the shared empty one included.
    #[must_use]
    pub fn interned_count(&self) -> u32 {
        self.interned_count
    }

    /// The temporary copies made so far.
    #[must_use]
    pub fn temporary_count(&self) -> u32 {
        self.temporary_count
    }

    /// The reports collected since the last call.
    pub fn take_reports(&mut self) -> Vec<StringCacheReport> {
        core::mem::take(&mut self.reports)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_spelling_wins() {
        let mut cache = StringCache::new();
        let a = cache.alloc(b"HpMount", false);
        let b = cache.alloc(b"hpmount", false);
        assert_eq!(a, b);
        assert_eq!(cache.get(b), b"HpMount");
        let t = cache.alloc(b"hpmount", true);
        assert_ne!(t, a);
        assert_eq!(cache.get(t), b"hpmount");
        assert!(cache.alloc(b"", false).is_empty());
        assert!(cache.alloc(b"\0x", true).is_empty());
        let bay = cache.alloc(b"bay", false);
        assert_eq!(bay, cache.cargo_bay());
        assert_eq!(cache.get(bay), b"BAY");
        assert_ne!(cache.alloc(b"BAY", true), cache.cargo_bay());
        cache.lowercase_in_place(a);
        assert_eq!(cache.get(b), b"hpmount");
        assert_eq!(cache.alloc(b"HPMOUNT", false), a);
    }

    #[test]
    fn overflow_returns_the_empty_string() {
        let mut cache = StringCache::new();
        let big = vec![b'a'; (POOL_BYTES - 6) as usize];
        let copy = cache.alloc(&big, true);
        assert_eq!(cache.get(copy).len(), big.len());
        assert_eq!(cache.free_bytes(), 0);
        assert!(cache.alloc(b"x", false).is_empty());
        assert_eq!(
            cache.take_reports(),
            [StringCacheReport::Overflow { temporary: false }]
        );
    }

    #[test]
    fn shutdown_forgets_the_index() {
        let mut cache = StringCache::new();
        let first = cache.alloc(b"Name", false);
        cache.shutdown();
        let second = cache.alloc(b"name", false);
        assert_ne!(first, second);
        assert_eq!(cache.get(second), b"name");
        assert_eq!(cache.interned_count(), 4);
    }
}
