//! The static half of the reputation vibes (`Reputation::Vibe`) and the
//! pass that fills it from the system files
//! (`SolarReps::LoadObjectVibesFromSystemIni`), ported from
//! `CppSource/Common/common_vibe.cpp` and `common_cobjdb.cpp`.
//!
//! While the universe loads (`BeginStaticLoad` to `EndStaticLoad`) every
//! `[Object]` of every system file allocates a vibe under its nickname id:
//! affiliation (the `group`, when that faction is loaded), name, information
//! card and the rank of its solar archetype. The records are appended to a
//! vector, found by a linear scan while loading, and sorted at the end with
//! the VC6 `std::sort` and a comparator that subtracts the keys. The 32-bit
//! difference wraps, so the order is not transitive: which keys the later
//! `bsearch` finds depends on the exact algorithms, and both are reproduced
//! here, with the one-entry lookup cache in front of them.
//!
//! The dynamic store (vibes of ships and players made at run time), the
//! feelings and the attitude rules belong to the object model and the
//! server (P2.4, P3).

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::Reports;
use super::archetype::ArchetypeDb;
use super::reputation::Reputation;
use crate::protocol::fmtstr::FmtStr;

/// The affiliation of a vibe without a (loaded) group.
pub const NO_AFFILIATION: u32 = 0xFFFF_FFFF;

/// The vector capacity `BeginStaticLoad` reserves.
const STATIC_CAPACITY: usize = 0x1000;

/// The VC6 `std::sort` threshold.
const SORT_MAX: usize = 16;

/// One vibe's data (`VibeValue`), as far as the static load fills it.
#[derive(Clone, Debug, PartialEq)]
pub struct VibeValue {
    /// The faction id, [`NO_AFFILIATION`] for none.
    pub affiliation: u32,
    /// The information card.
    pub infocard: u32,
    /// The rank (the solar archetype's toughness).
    pub rank: u32,
    /// The first name (`begin_mad_lib(ids_name)`).
    pub name1: FmtStr,
    /// The second name (empty).
    pub name2: FmtStr,
}

impl Default for VibeValue {
    fn default() -> Self {
        Self {
            affiliation: NO_AFFILIATION,
            infocard: 0,
            rank: 0,
            name1: FmtStr::default(),
            name2: FmtStr::default(),
        }
    }
}

/// A slot of the static store.
#[derive(Clone, Debug, PartialEq)]
pub struct VibeSlot {
    /// The nickname id, as a signed key.
    pub key: i32,
    /// The record.
    pub value: VibeValue,
}

/// The static vibe store.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct VibeDb {
    /// The slots, in append order while loading and in VC6 sort order after.
    pub slots: Vec<VibeSlot>,
    /// Loading mode (`BeginStaticLoad` until `EndStaticLoad`).
    loading: bool,
    /// The slot the last lookup returned (`GStaticVibeHit`). The sort moves
    /// records but not this index, as the C++ keeps its pointer.
    hit: Option<usize>,
}

/// The key comparator of the sort and of `bsearch`: the wrapping
/// difference.
fn key_diff(a: i32, b: i32) -> i32 {
    a.wrapping_sub(b)
}

fn before(a: &VibeSlot, b: &VibeSlot) -> bool {
    key_diff(a.key, b.key) < 0
}

impl VibeDb {
    /// `Vibe::Count`: the static slots (the dynamic store is empty here).
    #[must_use]
    pub fn count(&self) -> usize {
        self.slots.len()
    }

    /// `FreeStatic`: every slot dropped, the cache cleared.
    pub fn free_static(&mut self) {
        self.slots.clear();
        self.hit = None;
    }

    /// `BeginStaticLoad`.
    pub fn begin_static_load(&mut self) {
        self.loading = true;
        if self.slots.capacity() < STATIC_CAPACITY {
            self.slots.reserve(STATIC_CAPACITY - self.slots.len());
        }
    }

    /// `EndStaticLoad`: loading mode off and the slots sorted.
    pub fn end_static_load(&mut self) {
        self.loading = false;
        vc6_sort(&mut self.slots, before);
    }

    /// `FindStaticVibe`: the cached slot when its key matches, else a linear
    /// scan while loading or `bsearch` after; the result (a miss included)
    /// becomes the cache.
    pub fn find(&mut self, key: i32) -> Option<usize> {
        if let Some(index) = self.hit
            && self.slots.get(index).is_some_and(|slot| slot.key == key)
        {
            return Some(index);
        }
        let found = if self.loading {
            self.slots.iter().position(|slot| slot.key == key)
        } else {
            bsearch(&self.slots, |slot| key_diff(key, slot.key))
        };
        self.hit = found;
        found
    }

    /// `Vibe::Verify`.
    pub fn verify(&mut self, key: i32) -> bool {
        self.find(key).is_some()
    }

    /// The record of `key` (through the cache), if any.
    pub fn get(&mut self, key: i32) -> Option<&VibeValue> {
        let index = self.find(key)?;
        Some(&self.slots[index].value)
    }

    /// `Vibe::Alloc` for the static store: -1 is refused; a key already
    /// there has its record reset, a new one is appended. Returns -1 or 0.
    pub fn alloc(&mut self, key: i32) -> i32 {
        if key == -1 {
            return -1;
        }
        if self.loading {
            match self.find(key) {
                Some(index) => self.slots[index].value = VibeValue::default(),
                None => {
                    self.slots.push(VibeSlot {
                        key,
                        value: VibeValue::default(),
                    });
                    self.hit = Some(self.slots.len() - 1);
                }
            }
        }
        // Outside the static load `Alloc` fills the dynamic store, which is
        // not part of this module.
        0
    }

    fn value_mut(&mut self, key: i32) -> Option<&mut VibeValue> {
        let index = self.find(key)?;
        Some(&mut self.slots[index].value)
    }

    /// `Vibe::SetAffiliation(key, affiliation, false)`: -2 for no vibe, -3
    /// for a faction that is not loaded, else 0.
    pub fn set_affiliation(&mut self, key: i32, affiliation: u32, reputation: &Reputation) -> i32 {
        let Some(value) = self.value_mut(key) else {
            return -2;
        };
        if reputation.group(affiliation).is_none() {
            return -3;
        }
        value.affiliation = affiliation;
        0
    }

    /// `Vibe::SetName(key, name1, name2, nullptr)`.
    pub fn set_name(&mut self, key: i32, name1: &FmtStr, name2: &FmtStr) -> i32 {
        let Some(value) = self.value_mut(key) else {
            return -2;
        };
        value.name1 = name1.clone();
        value.name2 = name2.clone();
        0
    }

    /// `Vibe::SetInfocard`.
    pub fn set_infocard(&mut self, key: i32, infocard: u32) -> i32 {
        let Some(value) = self.value_mut(key) else {
            return -2;
        };
        value.infocard = infocard;
        0
    }

    /// `Vibe::SetRank`.
    pub fn set_rank(&mut self, key: i32, rank: u32) -> i32 {
        let Some(value) = self.value_mut(key) else {
            return -2;
        };
        value.rank = rank;
        0
    }

    /// `SolarReps::LoadObjectVibesFromSystemIni`: every `[Object]` section
    /// of an open system file, from where the reader stands.
    pub fn load_object_vibes(&mut self, reader: &mut IniReader, cx: &mut VibeCx<'_>) {
        while reader.read_header() {
            if reader.is_header(b"Object") {
                self.load_object_section(reader, cx);
            }
        }
    }

    /// `LoadOneObjectSection`: reading stops once a name, a nickname and a
    /// group are known; a section without a nickname allocates nothing.
    fn load_object_section(&mut self, reader: &mut IniReader, cx: &mut VibeCx<'_>) {
        let mut nickname = 0u32;
        let mut group: Vec<u8> = Vec::new();
        let mut ids_name = 0u32;
        let mut infocard = 0u32;
        let mut toughness = None;
        while reader.read_value() {
            if ids_name != 0 && nickname != 0 && !group.is_empty() {
                break;
            }
            if reader.is_value(b"nickname") {
                nickname = create_id(&reader.value_string());
            } else if reader.is_value(b"ids_name") {
                ids_name = u32::from_ne_bytes(reader.value_int(0).to_ne_bytes());
            } else if reader.is_value(b"infocard_ids") || reader.is_value(b"ids_info") {
                infocard = u32::from_ne_bytes(reader.value_int(0).to_ne_bytes());
            } else if reader.is_value(b"group") {
                group = super::goods::tstring(&reader.value_string(), 16);
            } else if reader.is_value(b"archetype") {
                let id = create_id(&reader.value_string());
                toughness = match cx.archetypes.solars.get(&id) {
                    Some(solar) => Some(solar.toughness),
                    None => {
                        cx.reports.push(
                            "Common\\Archetype.cpp",
                            0x49,
                            format!(
                                "*** ERROR: ArchDB::Get(0x{id:x}) failed. ID={}",
                                i32::from_ne_bytes(id.to_ne_bytes())
                            ),
                        );
                        None
                    }
                };
            }
        }
        if nickname == 0 {
            return;
        }
        let key = i32::from_ne_bytes(nickname.to_ne_bytes());
        self.alloc(key);
        let affiliation = cx.reputation.get_id(&group);
        self.set_affiliation(key, affiliation, cx.reputation);
        let name1 = FmtStr::new(ids_name);
        self.set_name(key, &name1, &FmtStr::default());
        self.set_infocard(key, infocard);
        if let Some(toughness) = toughness {
            self.set_rank(key, u32::from_ne_bytes(toughness.to_ne_bytes()));
        }
    }
}

/// What the object pass reads besides the file.
pub struct VibeCx<'a> {
    /// The solar archetypes (the rank).
    pub archetypes: &'a ArchetypeDb,
    /// The factions loaded so far (the affiliation).
    pub reputation: &'a Reputation,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

/// MSVCRT `bsearch` over `slots`, `compare` giving the key against a slot.
fn bsearch<T>(slots: &[T], compare: impl Fn(&T) -> i32) -> Option<usize> {
    let mut num = slots.len();
    if num == 0 {
        return None;
    }
    let mut lo = 0isize;
    let mut hi = isize::try_from(num).ok()? - 1;
    while lo <= hi {
        let half = num / 2;
        if half != 0 {
            let step = if num & 1 != 0 { half } else { half - 1 };
            let mid = lo + isize::try_from(step).ok()?;
            let index = usize::try_from(mid).ok()?;
            let result = compare(&slots[index]);
            if result == 0 {
                return Some(index);
            } else if result < 0 {
                hi = mid - 1;
                num = step;
            } else {
                lo = mid + 1;
                num = half;
            }
        } else if num != 0 {
            let index = usize::try_from(lo).ok()?;
            return (compare(&slots[index]) == 0).then_some(index);
        } else {
            break;
        }
    }
    None
}

/// The VC6 `std::sort` (`_Sort_0`, `_Sort`, `_Unguarded_partition`,
/// `_Median`, `_Insertion_sort`, `_Unguarded_insert`) step for step, with
/// `before` as `operator<`. The unguarded scans stop at the slice ends: the
/// C++ would read past them, which a comparator that is not a strict weak
/// order can make happen (see D152).
pub fn vc6_sort<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    let len = v.len();
    if len <= SORT_MAX {
        insertion_sort(v, before);
        return;
    }
    sort_range(v, before);
    insertion_sort(&mut v[..SORT_MAX], before);
    for index in SORT_MAX..len {
        let value = v[index].clone();
        unguarded_insert(v, index, value, before);
    }
}

fn sort_range<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    let (mut first, mut last) = (0usize, v.len());
    while last - first > SORT_MAX {
        let pivot = median(
            &v[first],
            &v[first + (last - first) / 2],
            &v[last - 1],
            before,
        )
        .clone();
        let mid = unguarded_partition(v, first, last, &pivot, before);
        if last - mid <= mid - first {
            sort_range(&mut v[mid..last], before);
            last = mid;
        } else {
            sort_range(&mut v[first..mid], before);
            first = mid;
        }
    }
}

fn median<'a, T>(x: &'a T, y: &'a T, z: &'a T, before: impl Fn(&T, &T) -> bool) -> &'a T {
    if before(x, y) {
        if before(y, z) {
            y
        } else if before(x, z) {
            z
        } else {
            x
        }
    } else if before(x, z) {
        x
    } else if before(y, z) {
        z
    } else {
        y
    }
}

fn unguarded_partition<T: Clone>(
    v: &mut [T],
    mut first: usize,
    mut last: usize,
    pivot: &T,
    before: impl Fn(&T, &T) -> bool,
) -> usize {
    loop {
        while first + 1 < v.len() && before(&v[first], pivot) {
            first += 1;
        }
        loop {
            if last == 0 {
                break;
            }
            last -= 1;
            if last == 0 || !before(pivot, &v[last]) {
                break;
            }
        }
        if last <= first {
            return first;
        }
        v.swap(first, last);
        first += 1;
    }
}

fn insertion_sort<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    for index in 1..v.len() {
        let value = v[index].clone();
        if before(&value, &v[0]) {
            v[..=index].rotate_right(1);
        } else {
            unguarded_insert(v, index, value, before);
        }
    }
}

fn unguarded_insert<T: Clone>(
    v: &mut [T],
    mut last: usize,
    value: T,
    before: impl Fn(&T, &T) -> bool,
) {
    while last > 0 && before(&value, &v[last - 1]) {
        v[last] = v[last - 1].clone();
        last -= 1;
    }
    v[last] = value;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bsearch_finds_in_sorted_order() {
        let v: Vec<i32> = (0..37).map(|x| x * 3).collect();
        for (index, &x) in v.iter().enumerate() {
            assert_eq!(bsearch(&v, |&s| x - s), Some(index));
            assert_eq!(bsearch(&v, |&s| x + 1 - s), None);
        }
        assert_eq!(bsearch::<i32>(&[], |_| 0), None);
    }

    #[test]
    fn vc6_sort_sorts_a_strict_order() {
        let mut state = 12345u32;
        let mut v: Vec<u32> = (0..300)
            .map(|_| {
                state = state.wrapping_mul(1_103_515_245).wrapping_add(12345);
                state >> 8
            })
            .collect();
        let mut want = v.clone();
        want.sort_unstable();
        vc6_sort(&mut v, |a, b| a < b);
        assert_eq!(v, want);
    }

    #[test]
    fn static_load_and_lookup() {
        let mut db = VibeDb::default();
        db.begin_static_load();
        for key in [5, -7, 9, 5] {
            db.alloc(key);
            db.set_infocard(key, 1);
        }
        db.end_static_load();
        assert_eq!(db.count(), 3);
        assert_eq!(
            db.slots.iter().map(|s| s.key).collect::<Vec<_>>(),
            [-7, 5, 9]
        );
        assert!(db.verify(9));
        assert!(!db.verify(10));
        assert_eq!(db.get(5).map(|v| v.infocard), Some(1));
        assert_eq!(db.alloc(-1), -1);
    }
}
