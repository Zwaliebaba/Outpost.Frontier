//! The string cache (`StringAlloc`, `CacheString`), ported from
//! `CppSource/Common/common_stringcache.cpp`.
//!
//! The C++ copies every string into one static pool of 0x18_0000 bytes and
//! hands out pointers into it. Interned strings are looked up first in an
//! index ordered by `stricmp`, so interning is case-insensitive: the first
//! spelling of a name wins and every later request with other letter case
//! gets that first copy back. Temporary strings are copied each time and
//! never looked up. Nothing is freed.
//!
//! The port keeps the two observable parts: the first-spelling rule and the
//! pool budget (both cursors), so an overflow happens at the same request
//! and yields the shared empty string as in the C++. A [`CacheString`] owns
//! a shared copy of its bytes instead of pointing into the pool.

use std::collections::HashMap;
use std::sync::Arc;

/// Bytes in the C++ pool (`StringCachePoolBytes`).
pub const POOL_BYTES: u32 = 0x18_0000;

/// A string handed out by the cache (`CacheString`): a C string without its
/// terminator. The default is the shared empty string.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CacheString(Arc<[u8]>);

impl CacheString {
    /// A string that did not come from a cache (fixed names in the code).
    #[must_use]
    pub fn from_static(text: &[u8]) -> Self {
        Self(Arc::from(neuron_core::hash::c_str(text)))
    }

    /// Whether both are the same copy: the C++ compares the pointers.
    #[must_use]
    pub fn same(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }

    /// The bytes, without a terminator.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }

    /// Whether this is the empty string (`Value[0] == '\0'`).
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// `CacheString::clear`: the shared empty string.
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    /// The bytes as text, with invalid UTF-8 replaced (for messages).
    #[must_use]
    pub fn to_lossy(&self) -> String {
        String::from_utf8_lossy(&self.0).into_owned()
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
            bottom: 1,
            top: POOL_BYTES,
            interned: HashMap::new(),
            interned_count: 1,
            temporary_count: 0,
            reports: Vec::new(),
            cargo_bay: CacheString::default(),
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
    /// An empty cache, as at process start.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `StringAlloc(text, temporary)`. `text` ends at its first NUL byte.
    pub fn alloc(&mut self, text: &[u8], temporary: bool) -> CacheString {
        let text = neuron_core::hash::c_str(text);
        if text.is_empty() {
            return CacheString::default();
        }
        let length = u32::try_from(text.len()).unwrap_or(u32::MAX);
        if temporary {
            if length.saturating_add(1) > self.top - self.bottom {
                self.reports
                    .push(StringCacheReport::Overflow { temporary: true });
                return CacheString::default();
            }
            self.top -= length + 1;
            self.temporary_count += 1;
            return CacheString(Arc::from(text));
        }
        let key = fold(text);
        if let Some(found) = self.interned.get(&key) {
            return found.clone();
        }
        if length.saturating_add(1) > self.top - self.bottom {
            self.reports
                .push(StringCacheReport::Overflow { temporary: false });
            return CacheString::default();
        }
        let copy = CacheString(Arc::from(text));
        self.interned.insert(key, copy.clone());
        self.interned_count += 1;
        self.bottom += length + 1;
        copy
    }

    /// `EquipDesc::CARGO_BAY_HP_NAME`: the internal cargo bay's hardpoint.
    /// An equipment line is internal exactly when its hardpoint is this
    /// copy ([`CacheString::same`]).
    #[must_use]
    pub fn cargo_bay(&self) -> &CacheString {
        &self.cargo_bay
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
        assert_eq!(a.as_bytes(), b"HpMount");
        assert_eq!(b.as_bytes(), b"HpMount");
        let t = cache.alloc(b"hpmount", true);
        assert_eq!(t.as_bytes(), b"hpmount");
        assert!(cache.alloc(b"", false).is_empty());
        assert!(cache.alloc(b"\0x", true).is_empty());
        let bay = cache.alloc(b"bay", false);
        assert!(bay.same(cache.cargo_bay()));
        assert_eq!(bay.as_bytes(), b"BAY");
        assert!(!cache.alloc(b"BAY", true).same(cache.cargo_bay()));
    }

    #[test]
    fn overflow_returns_the_empty_string() {
        let mut cache = StringCache::new();
        let big = vec![b'a'; (POOL_BYTES - 6) as usize];
        assert_eq!(cache.alloc(&big, true).as_bytes().len(), big.len());
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
        cache.alloc(b"Name", false);
        cache.shutdown();
        assert_eq!(cache.alloc(b"name", false).as_bytes(), b"name");
        assert_eq!(cache.interned_count(), 4);
    }
}
