//! `EquipDesc`: one line of equipment or cargo (`common_equipdesc.cpp`).

use super::string_cache::{CacheString, StringCache};

/// `SUBOBJ_ID_NONE`: the id of a line that has none yet.
pub const SUB_OBJECT_ID_NONE: u16 = 0;

/// `EquipDesc`, 0x20 bytes in the C++: a piece of equipment or cargo.
#[derive(Clone, Copy, Debug, PartialEq)]
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
            hardpoint: strings.cargo_bay(),
            equipped: 0,
            health: 1.0,
            count: 1,
            mission: false,
            owner: 0,
        }
    }

    /// `make_internal`: back into the cargo bay.
    pub fn make_internal(&mut self, strings: &StringCache) {
        self.hardpoint = strings.cargo_bay();
    }

    /// `is_internal`: the hardpoint is the cargo bay's own copy. A
    /// temporary copy of `BAY` is not internal, as in the C++.
    #[must_use]
    pub fn is_internal(&self, strings: &StringCache) -> bool {
        self.hardpoint == strings.cargo_bay()
    }

    /// `get_cargo_space_occupied`: `count * volume` for an internal line
    /// with an archetype, else 0. `volume` is the archetype's; the C++
    /// dereferences a missing archetype, here it counts as volume 0
    /// (divergence D116).
    #[must_use]
    pub fn cargo_space_occupied(&self, strings: &StringCache, volume: Option<f32>) -> f64 {
        if !self.is_internal(strings) || self.arch_id == 0 {
            return 0.0;
        }
        f64::from(self.count) * f64::from(volume.unwrap_or(0.0))
    }
}

/// `EquipDescList` (`Common/common_equipdesc.cpp:445-690`): a player's or a
/// record's equipment as a list, in list order. The list form of
/// `add_equipment_item` is the vector form's ([`super::loadout::add_equipment_item`]).
pub mod list {
    use super::super::goods::GoodList;
    use super::super::string_cache::CacheString;
    use super::EquipDesc;

    /// `remove_equipment_item(id, count)`: -1 when no item has the id; a
    /// count of -1 removes the whole item, any other is subtracted and the
    /// rest counts as undamaged (health 1); the item goes when its count
    /// reaches 0 (0), else stays (1).
    pub fn remove_equipment_item(items: &mut Vec<EquipDesc>, id: u16, count: i32) -> i32 {
        let Some(at) = items.iter().position(|i| i.id == id) else {
            return -1;
        };
        let item = &mut items[at];
        if count == -1 {
            item.count = 0;
        } else {
            item.count = item.count.wrapping_sub(count);
            item.health = 1.0;
        }
        if item.count == 0 {
            items.remove(at);
            0
        } else {
            1
        }
    }

    /// `find_equipment_item(id)`: the first item with the id.
    #[must_use]
    pub fn find_equipment_item(items: &[EquipDesc], id: u16) -> Option<usize> {
        items.iter().position(|i| i.id == id)
    }

    /// `find_equipment_item(hardpoint)`: the first item on the hardpoint (by
    /// handle, as the C++ compares the cached pointer).
    #[must_use]
    pub fn find_equipment_item_on(items: &[EquipDesc], hardpoint: CacheString) -> Option<usize> {
        items.iter().position(|i| i.hardpoint == hardpoint)
    }

    /// `traverse_equipment_type(type, prev)`: the first item of the
    /// archetype, or the next one after `prev`.
    #[must_use]
    pub fn traverse_equipment_type(
        items: &[EquipDesc],
        arch_id: u32,
        prev: Option<usize>,
    ) -> Option<usize> {
        let start = prev.map_or(0, |p| p + 1);
        items
            .iter()
            .enumerate()
            .skip(start)
            .find(|(_, i)| i.arch_id == arch_id)
            .map(|(at, _)| at)
    }

    /// `find_matching_cargo(type, count, health)`: for a combinable good the
    /// first item of it that is not a mission item and holds at least
    /// `count`; otherwise the unmounted, non-mission item whose health is
    /// closest below or above `health` (the first of equal ones; the
    /// difference `health - item health` is compared unrounded with the best
    /// so far, which starts at 2 and is stored as a float).
    #[must_use]
    pub fn find_matching_cargo(
        items: &[EquipDesc],
        goods: &GoodList,
        arch_id: u32,
        count: i32,
        health: f32,
    ) -> Option<usize> {
        if goods.arch_is_combinable(arch_id) {
            return items
                .iter()
                .position(|i| i.arch_id == arch_id && !i.mission && i.count >= count);
        }
        let mut best = None;
        let mut best_diff = 2.0f32;
        for (at, item) in items.iter().enumerate() {
            if item.arch_id != arch_id || item.mission || item.equipped != 0 {
                continue;
            }
            let diff = f64::from(health) - f64::from(item.health);
            if diff < f64::from(best_diff) {
                best_diff = diff as f32;
                best = Some(at);
            }
        }
        best
    }
}
