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
use super::vc6::{bsearch, vc6_sort};
use crate::protocol::fmtstr::FmtStr;

/// The affiliation of a vibe without a (loaded) group.
pub const NO_AFFILIATION: u32 = 0xFFFF_FFFF;

/// The vector capacity `BeginStaticLoad` reserves.
const STATIC_CAPACITY: usize = 0x1000;

/// One vibe's data (`VibeValue`): what the static load fills, and the
/// fields the run-time accessors keep (P3.2).
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
    /// `AttitudeTowardsPlayer` (+0x04), the value a multiplayer client
    /// reports.
    pub attitude_towards_player: f32,
    /// The owning client (+0x08), 0 for an NPC.
    pub client_id: u32,
    /// The cached faction record (+0x0c), by the faction's id: set by
    /// `SetAffiliation` and `Set`, cleared by `Alloc`, the multiplayer
    /// notify path and the auto-affiliation pass. The attitude blend reads
    /// it instead of the affiliation.
    pub affiliation_group: Option<u32>,
    /// The per-faction feeling overrides (+0x7c), in insertion order.
    pub group_feelings: Vec<(u32, f32)>,
    /// The display name (+0x44), at most 23 units.
    pub display_name: Vec<u16>,
    /// The attitude deltas (+0x88), head first: a new delta is put in front.
    pub deltas: Vec<(u32, f32)>,
}

impl Default for VibeValue {
    fn default() -> Self {
        Self {
            affiliation: NO_AFFILIATION,
            infocard: 0,
            rank: 0,
            name1: FmtStr::default(),
            name2: FmtStr::default(),
            attitude_towards_player: 0.0,
            client_id: 0,
            affiliation_group: None,
            group_feelings: Vec::new(),
            display_name: Vec::new(),
            deltas: Vec::new(),
        }
    }
}

impl VibeValue {
    /// `VibeValue::CopyFieldsFrom`: every field from `other`, except that
    /// the delta list keeps this record's own entries and gains `other`'s,
    /// each put in front in `other`'s order.
    fn copy_fields_from(&mut self, other: &Self) {
        self.affiliation = other.affiliation;
        self.affiliation_group = other.affiliation_group;
        self.client_id = other.client_id;
        for &delta in &other.deltas {
            self.deltas.insert(0, delta);
        }
        self.rank = other.rank;
        self.group_feelings.clone_from(&other.group_feelings);
        self.name1 = other.name1.clone();
        self.name2 = other.name2.clone();
        self.attitude_towards_player = other.attitude_towards_player;
        self.infocard = other.infocard;
        self.display_name = display_name(&other.display_name);
    }
}

/// `wcsncpy(.., 0x17)` with a NUL at 0x17: at most 23 units, cut at a NUL.
fn display_name(text: &[u16]) -> Vec<u16> {
    text.iter()
        .copied()
        .take_while(|&c| c != 0)
        .take(0x17)
        .collect()
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
    /// records but not this index, as the C++ keeps its pointer. Shared
    /// mutable, so that a lookup through a shared store (the AI's
    /// `get_toughness`) moves it as every lookup of the reference does.
    hit: HitCache,
}

/// `GStaticVibeHit` as an index that a lookup through `&VibeDb` can move
/// (the store is shared read-only by the loaded game data).
#[derive(Debug)]
struct HitCache(std::sync::atomic::AtomicUsize);

impl HitCache {
    const NONE: usize = usize::MAX;

    fn get(&self) -> Option<usize> {
        let index = self.0.load(std::sync::atomic::Ordering::Relaxed);
        (index != Self::NONE).then_some(index)
    }

    fn set(&self, index: Option<usize>) {
        self.0.store(
            index.unwrap_or(Self::NONE),
            std::sync::atomic::Ordering::Relaxed,
        );
    }
}

impl Default for HitCache {
    fn default() -> Self {
        Self(std::sync::atomic::AtomicUsize::new(Self::NONE))
    }
}

impl Clone for HitCache {
    fn clone(&self) -> Self {
        Self(std::sync::atomic::AtomicUsize::new(
            self.0.load(std::sync::atomic::Ordering::Relaxed),
        ))
    }
}

impl PartialEq for HitCache {
    fn eq(&self, other: &Self) -> bool {
        self.get() == other.get()
    }
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
        self.hit.set(None);
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
    pub fn find(&self, key: i32) -> Option<usize> {
        if let Some(index) = self.hit.get()
            && self.slots.get(index).is_some_and(|slot| slot.key == key)
        {
            return Some(index);
        }
        let found = if self.loading {
            self.slots.iter().position(|slot| slot.key == key)
        } else {
            bsearch(&self.slots, |slot| key_diff(key, slot.key))
        };
        self.hit.set(found);
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
                Some(index) => self.slots[index]
                    .value
                    .copy_fields_from(&VibeValue::default()),
                None => {
                    self.slots.push(VibeSlot {
                        key,
                        value: VibeValue::default(),
                    });
                    self.hit.set(Some(self.slots.len() - 1));
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
        if value.affiliation != affiliation {
            value.affiliation = affiliation;
            value.affiliation_group = Some(affiliation);
        }
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

/// Which side the process plays, as `SinglePlayer()` and `IsMPServer()`
/// tell the vibe accessors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VibeMode {
    /// Single player: the authoritative blend, no affiliation notices.
    SinglePlayer,
    /// The multiplayer server: the authoritative blend, notices recorded.
    MultiplayerServer,
    /// A multiplayer client: attitudes are the reported value.
    MultiplayerClient,
}

impl VibeMode {
    fn authoritative(self) -> bool {
        self != Self::MultiplayerClient
    }
}

/// `Vibe::Get`'s answer.
#[derive(Clone, Debug, PartialEq)]
pub struct VibeSummary {
    /// The affiliation.
    pub affiliation: u32,
    /// The rank.
    pub rank: u32,
    /// The feeling overrides as `Get` writes them: the count is a byte that
    /// wraps at 256, and a later entry overwrites the slot it wraps onto.
    pub relations: Vec<(u32, f32)>,
    /// The first name.
    pub name1: FmtStr,
    /// The second name.
    pub name2: FmtStr,
    /// The display name.
    pub display_name: Vec<u16>,
}

/// The vibe store of a running game (`Reputation::Vibe`,
/// `CppSource/Common/common_vibe.cpp`): the static store the universe load
/// fills, the dynamic records ships and players get at run time, and the
/// map of clients whose affiliation changed. Lookups try the dynamic store
/// first, except for the client id, which only it holds.
///
/// The C++ caches the last dynamic node a lookup found; a `std::map` find
/// is exact, so the port looks up without the cache. The static store's
/// cache is kept, because it can find a key the wrapping `bsearch` misses.
#[derive(Clone, Debug)]
pub struct Vibes {
    /// The static store.
    pub statics: VibeDb,
    /// The dynamic records, by signed key.
    pub dynamic: std::collections::BTreeMap<i32, VibeValue>,
    /// `GetChangedAffiliationClientMap`: client id to its previous
    /// affiliation.
    pub changed_affiliations: std::collections::BTreeMap<u32, u32>,
    /// The process's side.
    pub mode: VibeMode,
}

/// `kAffiliationKeepFeeling`.
const KEEP_FEELING: f32 = 0.65;
/// `kAffiliationJoinFeeling`.
const JOIN_FEELING: f32 = 0.9;

/// The record a lookup found.
#[derive(Clone, Copy)]
enum Slot {
    Dynamic(i32),
    Static(usize),
}

impl Vibes {
    /// A store over the static records of the universe load.
    #[must_use]
    pub fn new(statics: VibeDb, mode: VibeMode) -> Self {
        Self {
            statics,
            dynamic: std::collections::BTreeMap::new(),
            changed_affiliations: std::collections::BTreeMap::new(),
            mode,
        }
    }

    fn find(&self, key: i32) -> Option<Slot> {
        if self.dynamic.contains_key(&key) {
            return Some(Slot::Dynamic(key));
        }
        self.statics.find(key).map(Slot::Static)
    }

    fn value(&self, slot: Slot) -> &VibeValue {
        match slot {
            Slot::Dynamic(key) => &self.dynamic[&key],
            Slot::Static(index) => &self.statics.slots[index].value,
        }
    }

    fn value_mut(&mut self, slot: Slot) -> &mut VibeValue {
        match slot {
            Slot::Dynamic(key) => self.dynamic.get_mut(&key).expect("found"),
            Slot::Static(index) => &mut self.statics.slots[index].value,
        }
    }

    /// The record of `key`, dynamic first.
    pub fn get_value(&mut self, key: i32) -> Option<&VibeValue> {
        let slot = self.find(key)?;
        Some(self.value(slot))
    }

    /// `Vibe::Count`.
    #[must_use]
    pub fn count(&self) -> usize {
        self.statics.slots.len() + self.dynamic.len()
    }

    /// `Vibe::Verify`.
    pub fn verify(&mut self, key: i32) -> bool {
        self.find(key).is_some()
    }

    /// `Vibe::Alloc`: -1 for key -1; while the universe loads, the static
    /// store; afterwards a dynamic record (an existing one is reset but
    /// keeps its deltas; a key with a static slot gets a dynamic record that
    /// later lookups find first). 0.
    pub fn alloc(&mut self, key: i32) -> i32 {
        if key == -1 {
            return -1;
        }
        if self.statics.loading {
            return self.statics.alloc(key);
        }
        self.dynamic
            .entry(key)
            .or_default()
            .copy_fields_from(&VibeValue::default());
        0
    }

    /// `Vibe::EnsureExists`: `Alloc` when no record has the key. 0.
    pub fn ensure_exists(&mut self, key: i32) -> i32 {
        if self.find(key).is_none() {
            self.alloc(key);
        }
        0
    }

    /// `Vibe::Free`: only a dynamic record is freed (-2 otherwise); every
    /// other record then drops its deltas towards the key.
    pub fn free(&mut self, key: i32) -> i32 {
        if self.dynamic.remove(&key).is_none() {
            return -2;
        }
        let target = key as u32;
        for value in self.dynamic.values_mut() {
            value.deltas.retain(|&(id, _)| id != target);
        }
        for slot in &mut self.statics.slots {
            slot.value.deltas.retain(|&(id, _)| id != target);
        }
        0
    }

    /// `Vibe::FreeDynamic`: the static records drop their deltas towards
    /// every dynamic key, then the dynamic store is emptied.
    pub fn free_dynamic(&mut self) {
        for &key in self.dynamic.keys() {
            let target = key as u32;
            for slot in &mut self.statics.slots {
                slot.value.deltas.retain(|&(id, _)| id != target);
            }
        }
        self.dynamic.clear();
    }

    /// `Vibe::GetAffiliation`: (-2, none) for no record; a player-owned
    /// record answers none when `exclude_players`.
    pub fn affiliation(&mut self, key: i32, exclude_players: bool) -> (i32, u32) {
        let Some(slot) = self.find(key) else {
            return (-2, NO_AFFILIATION);
        };
        let v = self.value(slot);
        if exclude_players && v.client_id != 0 {
            (0, NO_AFFILIATION)
        } else {
            (0, v.affiliation)
        }
    }

    /// `Vibe::SetAffiliation`: -2 for no record, -3 for an unknown faction,
    /// 0. The faction record is cached unless a multiplayer server notifies,
    /// which records the client's previous affiliation instead.
    pub fn set_affiliation(
        &mut self,
        key: i32,
        affiliation: u32,
        notify: bool,
        rep: &Reputation,
    ) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        if rep.group(affiliation).is_none() {
            return -3;
        }
        let single = self.mode == VibeMode::SinglePlayer;
        let v = self.value_mut(slot);
        let old = v.affiliation;
        if old == affiliation {
            return 0;
        }
        v.affiliation = affiliation;
        if single || !notify {
            v.affiliation_group = Some(affiliation);
            return 0;
        }
        v.affiliation_group = None;
        let client = v.client_id;
        self.changed_affiliations.insert(client, old);
        0
    }

    /// `Vibe::GetClientID`: the dynamic store only; 0 for none.
    #[must_use]
    pub fn client_id(&self, key: i32) -> u32 {
        self.dynamic.get(&key).map_or(0, |v| v.client_id)
    }

    /// `Vibe::SetClientID`: the dynamic store only; -2 for none.
    pub fn set_client_id(&mut self, key: i32, client: u32) -> i32 {
        match self.dynamic.get_mut(&key) {
            Some(v) => {
                v.client_id = client;
                0
            }
            None => -2,
        }
    }

    /// `Vibe::GetInfocard`.
    pub fn infocard(&mut self, key: i32) -> (i32, u32) {
        match self.find(key) {
            Some(slot) => (0, self.value(slot).infocard),
            None => (-2, 0),
        }
    }

    /// `Vibe::SetInfocard`.
    pub fn set_infocard(&mut self, key: i32, infocard: u32) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        self.value_mut(slot).infocard = infocard;
        0
    }

    /// `Vibe::GetRank` (the static store's lookup cache moves).
    pub fn rank(&self, key: i32) -> (i32, u32) {
        match self.find(key) {
            Some(slot) => (0, self.value(slot).rank),
            None => (-2, 0),
        }
    }

    /// `Vibe::SetRank`.
    pub fn set_rank(&mut self, key: i32, rank: u32) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        self.value_mut(slot).rank = rank;
        0
    }

    /// `Vibe::GetName`: the two names and the display name, or `None` (-2).
    pub fn name(&mut self, key: i32) -> Option<(FmtStr, FmtStr, Vec<u16>)> {
        let slot = self.find(key)?;
        let v = self.value(slot);
        Some((v.name1.clone(), v.name2.clone(), v.display_name.clone()))
    }

    /// `Vibe::SetName`: a `None` display name empties it.
    pub fn set_name(
        &mut self,
        key: i32,
        name1: &FmtStr,
        name2: &FmtStr,
        display: Option<&[u16]>,
    ) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        let v = self.value_mut(slot);
        v.name1 = name1.clone();
        v.name2 = name2.clone();
        v.display_name = display.map(display_name).unwrap_or_default();
        0
    }

    /// `Vibe::SetAttitudeTowardsPlayer`.
    pub fn set_attitude_towards_player(&mut self, key: i32, attitude: f32) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        self.value_mut(slot).attitude_towards_player = attitude;
        0
    }

    /// `Vibe::GetGroupFeelingsTowards`: the faction feeling of the record's
    /// affiliation towards `other` (0 for a player's record) plus the
    /// record's override, clamped to -1..=1. (-2, 0) for no record, (-3, the
    /// value left) when the faction lookup fails.
    pub fn group_feelings_towards(&mut self, key: i32, other: u32, rep: &Reputation) -> (i32, f32) {
        let Some(slot) = self.find(key) else {
            return (-2, 0.0);
        };
        let v = self.value(slot);
        let (result, feeling) = rep.feelings_towards(other, v.affiliation);
        if result != 0 {
            return (-3, feeling.unwrap_or(0.0));
        }
        let mut out = feeling.unwrap_or(0.0);
        if v.client_id != 0 {
            out = 0.0;
        }
        let over = if other == NO_AFFILIATION {
            0.0
        } else {
            v.group_feelings
                .iter()
                .find(|(id, _)| *id == other)
                .map_or(0.0, |&(_, f)| f)
        };
        // `1.0 < out` then `-1.0 > out`: NaN passes through, as `clamp`
        // keeps it.
        out = ((f64::from(over) + f64::from(out)) as f32).clamp(-1.0, 1.0);
        (0, out)
    }

    /// `Vibe::SetGroupFeelingsTowards`: stores the override that makes the
    /// record's feeling towards `other` equal `feeling` (clamped), then, for
    /// a player's record, drops an affiliation it no longer likes above
    /// 0.65 and runs the auto-affiliation pass.
    pub fn set_group_feelings_towards(
        &mut self,
        key: i32,
        other: u32,
        feeling: f32,
        rep: &Reputation,
    ) -> i32 {
        let Some(slot) = self.find(key) else {
            return -2;
        };
        let mode = self.mode;
        let v = self.value_mut(slot);
        let mut result = -3;
        let (code, base) = rep.feelings_towards(other, v.affiliation);
        if code == 0 {
            result = 0;
            let base = if v.client_id != 0 {
                0.0
            } else {
                base.unwrap_or(0.0)
            };
            let delta = (f64::from(feeling) - f64::from(base)) as f32;
            if rep.group(other).is_some() {
                let mut clamped = if 1.0 < delta { 1.0 } else { delta };
                if clamped < -1.0 {
                    clamped = -1.0;
                }
                match v.group_feelings.iter_mut().find(|(id, _)| *id == other) {
                    Some(entry) => entry.1 = clamped,
                    None => v.group_feelings.push((other, clamped)),
                }
            }
        }
        if v.client_id == 0 {
            return result;
        }
        if v.affiliation != NO_AFFILIATION {
            let own = v
                .group_feelings
                .iter()
                .find(|(id, _)| *id == v.affiliation)
                .map_or(0.0, |&(_, f)| f);
            // `!(own <= 0.65)`: NaN keeps it too.
            if own.is_nan() || own > KEEP_FEELING {
                return result;
            }
            v.affiliation = NO_AFFILIATION;
        }
        let changed = update_player_affiliation(v, mode);
        if let Some((client, old)) = changed {
            self.changed_affiliations.insert(client, old);
        }
        result
    }

    /// `Vibe::Get`, or `None` (-2).
    pub fn summary(&mut self, key: i32) -> Option<VibeSummary> {
        let slot = self.find(key)?;
        let v = self.value(slot);
        // `Relations[count++]` with a byte count: entry j lands in slot
        // j % 256, and the caller reads `len % 256` slots.
        let mut slots: Vec<(u32, f32)> = Vec::new();
        for (j, &entry) in v.group_feelings.iter().enumerate() {
            let at = j % 256;
            if at < slots.len() {
                slots[at] = entry;
            } else {
                slots.push(entry);
            }
        }
        slots.truncate(v.group_feelings.len() % 256);
        let relations = slots;
        Some(VibeSummary {
            affiliation: v.affiliation,
            rank: v.rank,
            relations,
            name1: v.name1.clone(),
            name2: v.name2.clone(),
            display_name: v.display_name.clone(),
        })
    }

    /// `Vibe::Set`: -1 for key -1, -2 for no record, else 0. An NPC's record
    /// caches the faction record (none for an unknown faction); the
    /// overrides keep only known factions; then the auto-affiliation pass.
    #[allow(clippy::too_many_arguments)]
    pub fn set(
        &mut self,
        key: i32,
        affiliation: u32,
        rank: u32,
        relations: &[(u32, f32)],
        name1: &FmtStr,
        name2: &FmtStr,
        display: &[u16],
        rep: &Reputation,
    ) -> i32 {
        if key == -1 {
            return -1;
        }
        let Some(slot) = self.find(key) else {
            return -2;
        };
        let mode = self.mode;
        let v = self.value_mut(slot);
        v.affiliation = affiliation;
        if v.client_id == 0 {
            v.affiliation_group = rep.group(affiliation).map(|_| affiliation);
        }
        v.rank = rank;
        v.group_feelings = relations
            .iter()
            .copied()
            .filter(|(id, _)| rep.group(*id).is_some())
            .collect();
        v.name1 = name1.clone();
        v.name2 = name2.clone();
        v.display_name = display_name(display);
        if let Some((client, old)) = update_player_affiliation(v, mode) {
            self.changed_affiliations.insert(client, old);
        }
        0
    }

    /// `Vibe::AttitudeTowards`: (-2, 0) without `from`, (-3, 0) without
    /// `to`.
    pub fn attitude_towards(&mut self, from: i32, to: i32, rep: &Reputation) -> (i32, f32) {
        let (code, out, _) = self.attitude_core(from, to, false, rep);
        (code, out)
    }

    /// `Vibe::AttitudeTowardsSymmetrical`: both directions; the second is
    /// left `None` when a lookup fails.
    pub fn attitude_towards_symmetrical(
        &mut self,
        from: i32,
        to: i32,
        rep: &Reputation,
    ) -> (i32, f32, Option<f32>) {
        self.attitude_core(from, to, true, rep)
    }

    fn attitude_core(
        &mut self,
        from: i32,
        to: i32,
        both: bool,
        rep: &Reputation,
    ) -> (i32, f32, Option<f32>) {
        let Some(from_slot) = self.find(from) else {
            return (-2, 0.0, None);
        };
        let Some(to_slot) = self.find(to) else {
            return (-3, 0.0, None);
        };
        let mode = self.mode;
        let a = self.value(from_slot);
        let b = self.value(to_slot);
        let out1 = quantize(clamp(blend(a, b, to, mode, rep)));
        let out2 = both.then(|| quantize(clamp(blend(b, a, from, mode, rep))));
        (0, out1, out2)
    }

    /// `Vibe::AdjustAttitude`: the `from` record's delta towards `to` (a new
    /// one at the front when it has none) becomes `delta` minus the
    /// attitude `to`'s faction and override give `from`'s faction, clamped
    /// (unclamped when the faction lookup fails). -2 or -3 when a record is
    /// missing, else 0.
    pub fn adjust_attitude(&mut self, from: i32, to: i32, delta: f32, rep: &Reputation) -> i32 {
        let Some(from_slot) = self.find(from) else {
            return -2;
        };
        let Some(to_slot) = self.find(to) else {
            return -3;
        };
        let to_id = to as u32;
        let from_affiliation = self.value(from_slot).affiliation;
        let to_value = self.value(to_slot);
        let to_affiliation = to_value.affiliation;
        let to_client = to_value.client_id;
        let over = if from_affiliation == NO_AFFILIATION {
            0.0
        } else {
            to_value
                .group_feelings
                .iter()
                .find(|(id, _)| *id == from_affiliation)
                .map_or(0.0, |&(_, f)| f)
        };
        let (code, base) = rep.feelings_towards(from_affiliation, to_affiliation);
        let stored = if code == 0 {
            let base = if to_client != 0 {
                0.0
            } else {
                base.unwrap_or(0.0)
            };
            let sum = (f64::from(over) + f64::from(base)).clamp(-1.0, 1.0);
            (f64::from(delta) - sum) as f32
        } else {
            (f64::from(delta) - f64::from(base.unwrap_or(0.0))) as f32
        };
        let from_value = self.value_mut(from_slot);
        match from_value.deltas.iter_mut().find(|(id, _)| *id == to_id) {
            Some(entry) => entry.1 = stored,
            None => from_value.deltas.insert(0, (to_id, stored)),
        }
        0
    }
}

/// The player-only auto-affiliation pass: a record still liking its
/// affiliation above 0.65 keeps it; otherwise the faction it likes most
/// above 0.9 that is not a story faction, or none. On a change in
/// multiplayer, the client and its previous affiliation are returned for
/// the changed-affiliation map.
fn update_player_affiliation(v: &mut VibeValue, mode: VibeMode) -> Option<(u32, u32)> {
    if v.client_id == 0 {
        return None;
    }
    let mut best = JOIN_FEELING;
    let mut chosen = NO_AFFILIATION;
    for &(id, feeling) in &v.group_feelings {
        if id == v.affiliation && feeling > KEEP_FEELING {
            return None;
        }
        // `!(feeling > best)`: NaN is skipped too.
        if feeling.is_nan() || feeling <= best || Reputation::is_story_faction(id) {
            continue;
        }
        chosen = id;
        best = feeling;
    }
    let old = v.affiliation;
    if chosen == old {
        return None;
    }
    v.affiliation = chosen;
    v.affiliation_group = None;
    (mode != VibeMode::SinglePlayer).then_some((v.client_id, old))
}

/// `BlendAttitudeRaw`, in the x87 unit's double: a multiplayer client
/// answers the reported attitude; otherwise the cached faction record's
/// feeling towards `to`'s faction (1 for the same faction, 0 without a
/// cache or a faction, 0 towards a player), plus `to`'s override for
/// `from`'s faction, plus `from`'s delta towards the raw `to_key`.
fn blend(from: &VibeValue, to: &VibeValue, to_key: i32, mode: VibeMode, rep: &Reputation) -> f64 {
    if !mode.authoritative() {
        return f64::from(from.attitude_towards_player);
    }
    let mut base = 0.0f32;
    if let Some(group) = from.affiliation_group.and_then(|id| rep.group(id))
        && to.affiliation != NO_AFFILIATION
    {
        base = if from.affiliation == to.affiliation {
            1.0
        } else {
            group.feelings.get(&to.affiliation).copied().unwrap_or(0.0)
        };
    }
    if to.client_id != 0 {
        base = 0.0;
    }
    let over = if from.affiliation == NO_AFFILIATION {
        0.0
    } else {
        to.group_feelings
            .iter()
            .find(|(id, _)| *id == from.affiliation)
            .map_or(0.0, |&(_, f)| f)
    };
    let mut sum = f64::from(base) + f64::from(over);
    if let Some(&(_, d)) = from.deltas.iter().find(|(id, _)| *id == to_key as u32) {
        sum += f64::from(d);
    }
    sum
}

/// `ClampAttitude` (NaN passes through).
fn clamp(value: f64) -> f64 {
    value.clamp(-1.0, 1.0)
}

/// `QuantizeAttitude`: rounded half away from zero to hundredths, a
/// `1e-8` added away from zero, 0 within `1e-8`.
fn quantize(value: f64) -> f32 {
    let step = 1.0 / 0.01;
    let half = 0.5 / step;
    let epsilon = 1.0e-8;
    if value < -epsilon {
        (f64::from(neuron_core::num::trunc_to_i32((value - half) * step)) / step - epsilon) as f32
    } else if value > epsilon {
        (f64::from(neuron_core::num::trunc_to_i32((value + half) * step)) / step + epsilon) as f32
    } else {
        0.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
