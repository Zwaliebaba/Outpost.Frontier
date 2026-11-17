//! Fuses, the petal database and the weapon modifiers, ported from
//! `CppSource/Common/common_fuses.cpp`.
//!
//! `fuses` values of the `[Data]` list only record their paths
//! ([`FuseFiles`]; the C++ keeps the full path `GetFileSysPath` made, the
//! port the path relative to the data root); the server and the client read those files with their
//! own fuse readers, which build [`Fuse`]s whose actions (effects, damage,
//! detachments) belong to the objects (P2.4). `PetalDB` names the meshes of
//! the interface petals; `WeaponModDB` scales shield damage per weapon and
//! shield type.

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::rng::CrtRand;

use super::string_cache::{CacheString, StringCache};
use super::{DataSource, Reports, lossy, open_ini, value_f32};

const PETAL_SOURCE: &str = "Common\\PetalDB.cpp";
const WEAPON_MOD_SOURCE: &str = "Common\\WeaponModDB.cpp";

/// `FuseDB::m_FuseINIFiles`: the fuse files in `[Data]` order, each cut to
/// `TString<260>`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FuseFiles(pub Vec<Vec<u8>>);

impl FuseFiles {
    /// `FuseDB::AddFuseINIFile`.
    pub fn add(&mut self, path: &[u8]) {
        self.0.push(super::goods::tstring(path, 260));
    }

    /// `FuseDB::Shutdown`.
    pub fn clear(&mut self) {
        self.0.clear();
    }
}

/// `EPetalCategory`: room, spaceship, solar, asteroid, equipment,
/// equipment cart, prop.
pub const PETAL_CATEGORIES: usize = 7;

/// A petal record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PetalRecord {
    /// The nickname id.
    pub nickname: u32,
    /// The mesh path (at most 0x3FF bytes).
    pub mesh: Vec<u8>,
    /// Values 2 and on (at most 16), temporary string-cache copies.
    pub extras: Vec<CacheString>,
}

/// `PetalInterfaceDatabase`.
#[derive(Clone, Debug)]
pub struct PetalDb {
    /// The records of each category by nickname id.
    pub categories: [BTreeMap<u32, PetalRecord>; PETAL_CATEGORIES],
}

impl Default for PetalDb {
    fn default() -> Self {
        Self {
            categories: core::array::from_fn(|_| BTreeMap::new()),
        }
    }
}

/// `CopyZ` into a 0x400-byte buffer.
fn copy_z(text: &[u8]) -> Vec<u8> {
    super::goods::tstring(text, 0x400)
}

impl PetalDb {
    /// `get_category`: the category of a display name, or `None`
    /// (reported) for an unknown one; `None` without a report for an
    /// empty one.
    pub fn category(name: &[u8], reports: &mut Reports) -> Option<usize> {
        const NAMES: [&[u8]; PETAL_CATEGORIES] = [
            b"Room",
            b"Spaceship",
            b"Solar",
            b"Asteroid",
            b"Equipment",
            b"Equipment Cart",
            b"Prop",
        ];
        if name.is_empty() {
            return None;
        }
        let found = NAMES.iter().position(|n| *n == name);
        if found.is_none() {
            reports.push(
                PETAL_SOURCE,
                0xbb,
                format!("*** WARNING: Unknown Petal category name {}", lossy(name)),
            );
        }
        found
    }

    /// `find_record`.
    #[must_use]
    pub fn find(&self, category: usize, nickname: u32) -> Option<&PetalRecord> {
        self.categories.get(category)?.get(&nickname)
    }

    /// `load_from_ini(path)`: the `[ObjectTable]` values `room`, `cart`
    /// and `prop`; the first record for a nickname wins.
    pub fn load(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        strings: &mut StringCache,
        reports: &mut Reports,
    ) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                PETAL_SOURCE,
                0x86,
                format!("*** ERROR: Cannot open petal database file {}", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if !reader.is_header(b"ObjectTable") {
                continue;
            }
            while reader.read_value() {
                let name = reader.name();
                let category = if eq_ignore_case(&name, b"room") {
                    Some(0)
                } else if eq_ignore_case(&name, b"cart") {
                    Some(5)
                } else if eq_ignore_case(&name, b"prop") {
                    Some(6)
                } else {
                    reports.push(
                        PETAL_SOURCE,
                        0x59,
                        format!(
                            "*** ERROR: Invalid category '{}' in petal database file {}",
                            lossy(&name),
                            lossy(path)
                        ),
                    );
                    None
                };
                if reader.is_value_empty(0) || reader.is_value_empty(1) {
                    reports.push(
                        PETAL_SOURCE,
                        0x61,
                        format!(
                            "*** ERROR: Invalid PetalDB entry {} in .ini file {}, cannot find nickname and mesh",
                            lossy(&reader.value_string()),
                            lossy(path)
                        ),
                    );
                }
                let nickname = copy_z(&reader.value_string_at(0));
                let mesh = copy_z(&reader.value_string_at(1));
                let mut extras = Vec::new();
                let mut index = 2;
                while !reader.is_value_empty(index) && extras.len() < 0x10 {
                    extras.push(copy_z(&reader.value_string_at(index)));
                    index += 1;
                }
                if let Some(category) = category {
                    let id = create_id(&nickname);
                    let extras = extras.iter().map(|e| strings.alloc(e, true)).collect();
                    self.categories[category].entry(id).or_insert(PetalRecord {
                        nickname: id,
                        mesh,
                        extras,
                    });
                }
            }
        }
    }
}

/// `FWeaponType`: shield modifiers of one weapon type.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WeaponType {
    /// The nickname id.
    pub nickname: u32,
    /// Modifier by shield type; the first line for a shield wins.
    pub shield_mods: BTreeMap<u32, f32>,
}

/// The weapon modifier database (`WeaponModDB`).
#[derive(Clone, Debug, Default)]
pub struct WeaponModDb {
    /// Weapon types by id.
    pub types: BTreeMap<u32, WeaponType>,
}

impl WeaponModDb {
    /// `WeaponModDB_load(path)`: `[WeaponType]` blocks; a `shield_mod`
    /// before any `nickname` of its block is ignored.
    pub fn load(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                WEAPON_MOD_SOURCE,
                0x48,
                format!("*** ERROR: Failed to open WeaponMod file {}.", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if !reader.is_header(b"WeaponType") {
                continue;
            }
            let mut current: Option<u32> = None;
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    let id = create_id(&reader.value_string());
                    self.types.entry(id).or_insert_with(|| WeaponType {
                        nickname: id,
                        shield_mods: BTreeMap::new(),
                    });
                    current = Some(id);
                } else if reader.is_value(b"shield_mod")
                    && let Some(weapon) = current.and_then(|id| self.types.get_mut(&id))
                {
                    let shield = create_id(&reader.value_string_at(0));
                    let modifier = value_f32(&mut reader, 1);
                    weapon.shield_mods.entry(shield).or_insert(modifier);
                }
            }
        }
    }

    /// `WeaponModDB_GetShieldMod`: 1 when there is no entry.
    #[must_use]
    pub fn shield_mod(&self, weapon: u32, shield: u32) -> f32 {
        self.types
            .get(&weapon)
            .and_then(|w| w.shield_mods.get(&shield))
            .copied()
            .unwrap_or(1.0)
    }
}

/// `FuseAction`: the base of a fuse's timed actions.
#[derive(Clone, Debug, PartialEq)]
pub struct FuseAction {
    /// `at_t`: the trigger time as a fraction of the fuse's life.
    pub at_t: f32,
    /// The end of a random range, -1 for none.
    pub at_t_end: f32,
    /// `Load` / `UnLoad` count.
    pub ref_count: i32,
    /// Triggered at least once.
    pub triggered: bool,
    /// The sub-objects it was triggered for.
    pub triggered_sub_objs: Vec<u16>,
}

impl Default for FuseAction {
    fn default() -> Self {
        Self {
            at_t: -1.0,
            at_t_end: -1.0,
            ref_count: 0,
            triggered: false,
            triggered_sub_objs: Vec::new(),
        }
    }
}

impl FuseAction {
    /// `ReadFuseActionValue`: `at_t = time[, end]`; other keys are the
    /// caller's.
    pub fn read_value(&mut self, reader: &mut IniReader) -> bool {
        if !reader.is_value(b"at_t") {
            return false;
        }
        self.at_t = value_f32(reader, 0);
        if !reader.is_value_empty(1) {
            self.at_t_end = value_f32(reader, 1);
        }
        true
    }

    /// `IsTriggered(sub_obj)`.
    #[must_use]
    pub fn is_triggered_for(&self, sub_obj: u16) -> bool {
        self.triggered && self.triggered_sub_objs.contains(&sub_obj)
    }

    /// `Trigger`.
    pub fn trigger(&mut self, sub_obj: u16) {
        self.triggered = true;
        if !self.triggered_sub_objs.contains(&sub_obj) {
            self.triggered_sub_objs.push(sub_obj);
        }
    }

    /// `UnTrigger`: the flag stays.
    pub fn untrigger(&mut self, sub_obj: u16) {
        if let Some(index) = self.triggered_sub_objs.iter().position(|&s| s == sub_obj) {
            self.triggered_sub_objs.remove(index);
        }
    }

    /// `ShouldRandomize`.
    #[must_use]
    pub fn should_randomize(&self) -> bool {
        self.at_t_end != -1.0
    }

    /// `Randomize`: a time drawn between `at_t` and the range end.
    pub fn randomize(&mut self, rand: &mut CrtRand) -> f32 {
        let begin = self.at_t;
        let end = self.at_t_end;
        let roll = rand.rand();
        self.at_t_end = -1.0;
        let scaled = f64::from(roll) * f64::from(1.0_f32 / 32768.0);
        let span = f64::from(end) - f64::from(begin);
        self.at_t = (scaled * span + f64::from(begin)) as f32;
        self.at_t
    }
}

/// `FFuseBurn`: one ignition and the part of the fuse it has burnt.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FuseBurn {
    /// The igniting object.
    pub source: u32,
    /// The sub-object.
    pub sub_obj: u16,
    /// Where it was lit.
    pub ignite_t: f32,
    /// Burnt down to here.
    pub lower: f32,
    /// Burnt up to here.
    pub upper: f32,
}

/// `Fuse`: actions keyed by trigger time and the burns running on them.
#[derive(Clone, Debug, PartialEq)]
pub struct Fuse {
    /// `name`.
    pub nickname: u32,
    /// `lifetime` in seconds.
    pub lifetime: f32,
    /// `death_fuse`.
    pub death_fuse: bool,
    /// The actions by trigger time; equal times keep insertion order.
    pub actions: Vec<(f32, FuseAction)>,
    /// Actions still to be randomised.
    pub needs_randomize: bool,
    /// The burns.
    pub burns: Vec<FuseBurn>,
}

impl Fuse {
    /// `Fuse(nickname, lifetime)`.
    #[must_use]
    pub fn new(nickname: u32, lifetime: f32) -> Self {
        Self {
            nickname,
            lifetime,
            death_fuse: false,
            actions: Vec::new(),
            needs_randomize: false,
            burns: Vec::new(),
        }
    }

    /// Inserts an action under its time, after equal times (the multimap).
    pub fn add_action(&mut self, action: FuseAction) {
        let key = action.at_t;
        let at = self.actions.partition_point(|(k, _)| *k <= key);
        self.actions.insert(at, (key, action));
    }

    /// `ReadFuseValues`: drops the actions, then `name`, `lifetime`,
    /// `death_fuse`.
    pub fn read_values(&mut self, reader: &mut IniReader) -> bool {
        self.actions.clear();
        self.nickname = 0;
        self.lifetime = -1.0;
        self.death_fuse = false;
        while reader.read_value() {
            if reader.is_value(b"name") {
                self.nickname = create_id(&reader.value_string());
            } else if reader.is_value(b"lifetime") {
                self.lifetime = value_f32(reader, 0);
            } else if reader.is_value(b"death_fuse") {
                self.death_fuse = reader.value_bool(0);
            }
        }
        true
    }

    /// `IsBurning`.
    #[must_use]
    pub fn is_burning(&self) -> bool {
        !self.burns.is_empty()
    }

    /// `IsSpent`: a burn covers the whole fuse.
    #[must_use]
    pub fn is_spent(&self) -> bool {
        self.burns.iter().any(|b| b.lower <= 0.0 && b.upper >= 1.0)
    }

    /// `IsSpentAt`.
    #[must_use]
    pub fn is_spent_at(&self, t: f32) -> bool {
        if !(0.0..=1.0).contains(&t) {
            return false;
        }
        self.burns.iter().any(|b| b.lower <= t && t <= b.upper)
    }

    /// `IgniteAt`: `false` when that sub-object is already lit there.
    pub fn ignite_at(&mut self, source: u32, sub_obj: u16, at_t: f32) -> bool {
        if self
            .burns
            .iter()
            .any(|b| b.sub_obj == sub_obj && b.ignite_t == at_t)
        {
            return false;
        }
        self.burns.push(FuseBurn {
            source,
            sub_obj,
            ignite_t: at_t,
            lower: at_t,
            upper: at_t,
        });
        true
    }

    /// `Burn(dt)`: widens every burn and triggers the actions it covers.
    pub fn burn(&mut self, dt: f32) {
        if self.burns.is_empty() {
            return;
        }
        let step = (f64::from(dt) / f64::from(self.lifetime)) as f32;
        for b in &mut self.burns {
            let lower = f64::from(b.lower) - f64::from(step);
            b.lower = if lower < 0.0 { 0.0 } else { lower as f32 };
            let upper = f64::from(step) + f64::from(b.upper);
            b.upper = if upper > 1.0 { 1.0 } else { upper as f32 };
        }
        for b in &self.burns {
            for (key, action) in &mut self.actions {
                if *key >= b.lower && *key <= b.upper && !action.is_triggered_for(b.sub_obj) {
                    action.trigger(b.sub_obj);
                }
            }
        }
    }

    /// `UnBurn`: removes the burn lit at `at_t` and untriggers what it
    /// covers; `false` when there is none.
    pub fn unburn(&mut self, sub_obj: u16, at_t: f32) -> bool {
        let Some(index) = self
            .burns
            .iter()
            .position(|b| b.sub_obj == sub_obj && b.ignite_t == at_t)
        else {
            return false;
        };
        let b = self.burns[index];
        for (key, action) in &mut self.actions {
            if *key >= b.lower && *key <= b.upper && action.triggered {
                action.untrigger(b.sub_obj);
            }
        }
        self.burns.remove(index);
        true
    }

    /// `RandomizeActions`: every ranged action draws its time and moves to
    /// it, in key order.
    pub fn randomize_actions(&mut self, rand: &mut CrtRand) {
        if !self.needs_randomize {
            return;
        }
        let mut index = 0;
        while index < self.actions.len() {
            if self.actions[index].1.should_randomize() {
                let (_, mut action) = self.actions.remove(index);
                let key = action.randomize(rand);
                let at = self.actions.partition_point(|(k, _)| *k <= key);
                self.actions.insert(at, (key, action));
                if at <= index {
                    index += 1;
                }
            } else {
                index += 1;
            }
        }
        self.needs_randomize = false;
    }

    /// `OverrideLifetime`: refused while burning.
    pub fn override_lifetime(&mut self, lifetime: f32) -> bool {
        if !self.burns.is_empty() {
            return false;
        }
        self.lifetime = lifetime;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn burning() {
        let mut fuse = Fuse::new(1, 10.0);
        let action = FuseAction {
            at_t: 0.5,
            ..FuseAction::default()
        };
        fuse.add_action(action);
        assert!(fuse.ignite_at(7, 3, 0.4));
        assert!(!fuse.ignite_at(7, 3, 0.4));
        fuse.burn(0.5);
        assert!(!fuse.actions[0].1.triggered);
        fuse.burn(0.6);
        assert!(fuse.actions[0].1.is_triggered_for(3));
        assert!(fuse.unburn(3, 0.4));
        assert!(!fuse.actions[0].1.is_triggered_for(3));
        assert!(fuse.actions[0].1.triggered);
    }

    #[test]
    fn weapon_mods() {
        struct One(&'static [u8]);
        impl DataSource for One {
            fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
                Some(self.0.to_vec())
            }
        }
        let text = b"[WeaponType]\nshield_mod = s, 9\nnickname = w\nshield_mod = s, 0.5\nshield_mod = s, 2\n";
        let mut db = WeaponModDb::default();
        db.load(&One(text), b"w.ini", &mut Reports::default());
        assert_eq!(db.shield_mod(create_id(b"w"), create_id(b"s")), 0.5);
        assert_eq!(db.shield_mod(create_id(b"w"), create_id(b"t")), 1.0);
    }
}
