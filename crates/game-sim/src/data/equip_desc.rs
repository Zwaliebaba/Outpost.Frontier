//! `EquipDesc`: one line of equipment or cargo (`common_equipdesc.cpp`).

use super::string_cache::{CacheString, StringCache};

/// `SUBOBJ_ID_NONE`: the id of a line that has none yet.
pub const SUB_OBJECT_ID_NONE: u16 = 0;

/// `EquipDesc`, 0x20 bytes in the C++: a piece of equipment or cargo.
#[derive(Clone, Debug, PartialEq)]
pub struct EquipDesc {
    /// `Cached` (+0x00).
    pub cached: u16,
    /// The sub-object id (+0x02).
    pub id: u16,
    /// The equipment archetype (+0x04).
    pub arch_id: u32,
    /// The hardpoint (+0x08); the cargo bay `BAY` for internal items.
    pub hardpoint: CacheString,
    /// Mounted rather than carried (+0x0C, a raw byte).
    pub equipped: u8,
    /// Health, 1 when new (+0x10).
    pub health: f32,
    /// How many (+0x14), 1 by default.
    pub count: i32,
    /// A mission item (+0x18, `bMission`, `get_temporary`).
    pub mission: bool,
    /// The owning player (+0x1C).
    pub owner: u32,
}

impl EquipDesc {
    /// `EquipDesc()`: count 1, health 1, the cargo bay.
    #[must_use]
    pub fn new(strings: &StringCache) -> Self {
        Self {
            cached: 0,
            id: SUB_OBJECT_ID_NONE,
            arch_id: 0,
            hardpoint: strings.cargo_bay().clone(),
            equipped: 0,
            health: 1.0,
            count: 1,
            mission: false,
            owner: 0,
        }
    }

    /// `make_internal`: back into the cargo bay.
    pub fn make_internal(&mut self, strings: &StringCache) {
        self.hardpoint = strings.cargo_bay().clone();
    }

    /// `is_internal`: the hardpoint is the cargo bay's own copy. A
    /// temporary copy of `BAY` is not internal, as in the C++.
    #[must_use]
    pub fn is_internal(&self, strings: &StringCache) -> bool {
        self.hardpoint.same(strings.cargo_bay())
    }

    /// `get_cargo_space_occupied`: `count * volume` for an internal line
    /// with an archetype, else 0. `volume` is the archetype's; the C++
    /// dereferences a missing archetype, here it counts as volume 0.
    #[must_use]
    pub fn cargo_space_occupied(&self, strings: &StringCache, volume: Option<f32>) -> f64 {
        if !self.is_internal(strings) || self.arch_id == 0 {
            return 0.0;
        }
        f64::from(self.count) * f64::from(volume.unwrap_or(0.0))
    }
}
