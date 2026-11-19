//! A ship's cargo capacities (`CShip::get_cargo_hold_remaining`,
//! `get_repair_kit_capacity_remaining`,
//! `get_shield_battery_capacity_remaining`, `get_ammo_capacity_remaining`,
//! `get_space_for_cargo_type`; `Common/common_cship.cpp`) and the scanner's
//! cargo scan (`CEScanner::ScanCargo`, `GetEquipDesc` of each item;
//! `common_cescanner.cpp`, `common_cequip.cpp`, `common_cecargo.cpp`).
//!
//! The hold counts every item of the internal mask `0x1ff0000` (mounted
//! internal equipment its volume once, a cargo stack its count times its
//! volume), rounded to `float` at each sum. The repair kits and shield
//! batteries are limited by the ship archetype's `nanobot_limit` and
//! `shield_battery_limit` (-1: no limit), munitions, mines and
//! counter-measures by the constant `MAX_PLAYER_AMMO`; each limit is read
//! against the first cargo stack of the kind.

use crate::equip::{EquipKind, SubObjId, types};
use crate::object::ObjHandle;

use super::World;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `AClassRepairKit`.
pub const CLASS_REPAIR_KIT: u32 = 0x1a;
/// `AClassShieldBattery`.
pub const CLASS_SHIELD_BATTERY: u32 = 0x1b;
/// `AClassMunition`.
pub const CLASS_MUNITION: u32 = 0x1f;
/// `AClassMine`.
pub const CLASS_MINE: u32 = 0x20;
/// `AClassCounterMeasure`.
pub const CLASS_COUNTER_MEASURE: u32 = 0x21;

/// The internal-equipment mask the hold counts.
const HOLD_MASK: u32 = 0x1ff_0000;

/// `CEScanner::ScanCargo`'s answer (`ScanResult`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScanResult {
    /// 0: the target is beyond the cargo scan range.
    TooFar,
    /// 1: the owner's power is below the scanner's threshold.
    NotEnoughPower,
    /// 2: the list was filled.
    Complete,
    /// 3: no target.
    NoTarget,
}

/// One entry of a cargo scan: the `EquipDesc` `GetEquipDesc` fills.
#[derive(Clone, Debug, PartialEq)]
pub struct ScannedEquip {
    /// The sub-object id.
    pub id: SubObjId,
    /// The equipment archetype.
    pub arch_id: u32,
    /// The hardpoint (`set_hardpoint`), `None` for the cargo bay
    /// (`make_internal`).
    pub hardpoint: Option<Vec<u8>>,
    /// Mounted (1) or carried (0).
    pub equipped: bool,
    /// The health (`set_status`).
    pub health: f32,
    /// The count: a stack's, 1 for anything else.
    pub count: i32,
    /// A mission item.
    pub mission: bool,
}

impl World {
    /// The ship's archetype, for a live ship.
    fn ship_arch(&self, h: ObjHandle) -> Option<&crate::data::archetype::eq_obj::Ship> {
        let o = self.objects.get(h)?;
        o.as_ship()?;
        self.data.archetypes.ships.get(&o.archetype)
    }

    /// An equipment archetype's volume and class (0, 0 without one).
    fn equip_volume_class(&self, arch: u32) -> (f32, u32) {
        self.data
            .archetypes
            .equipment
            .get(&arch)
            .map_or((0.0, 0), |a| (a.volume, a.class()))
    }

    /// The hold's used volume: each item of the internal mask once, a
    /// cargo stack its count times, rounded to `float` at each sum.
    fn hold_used(&self, h: ObjHandle) -> Option<f32> {
        let eq = self.objects.get(h)?.as_equipped()?;
        let mut used = 0.0_f32;
        for slot in eq.equipment.traverse(HOLD_MASK, false) {
            let Some(item) = eq.equipment.get(slot) else {
                continue;
            };
            let (volume, _) = self.equip_volume_class(item.arch_id);
            used = match (&item.kind, item.type_bits) {
                (EquipKind::Cargo(c), types::CARGO) => r(f64::from(c.count) * e(volume) + e(used)),
                _ => r(e(used) + e(volume)),
            };
        }
        Some(used)
    }

    /// `CShip::get_cargo_hold_remaining`: the archetype's `hold_size` less
    /// the used volume, unrounded; 0 when the hold is over-full. `None` for
    /// an object that is not a ship.
    #[must_use]
    pub fn cargo_hold_remaining(&self, h: ObjHandle) -> Option<f64> {
        let hold = self.ship_arch(h)?.hold_size;
        let used = self.hold_used(h)?;
        Some(if used <= hold { e(hold) - e(used) } else { 0.0 })
    }

    /// The ship archetype's `hold_size`.
    #[must_use]
    pub fn cargo_hold_size(&self, h: ObjHandle) -> Option<f32> {
        Some(self.ship_arch(h)?.hold_size)
    }

    /// The limit less the count of the first cargo stack of class `class`
    /// (0 when the count is above it), the limit without such a stack, -1
    /// for no limit.
    fn kind_capacity_remaining(&self, h: ObjHandle, limit: i32, class: u32) -> Option<i32> {
        if limit == -1 {
            return Some(limit);
        }
        let eq = self.objects.get(h)?.as_equipped()?;
        for slot in eq.equipment.traverse(types::CARGO, false) {
            let Some(item) = eq.equipment.get(slot) else {
                continue;
            };
            if self.equip_volume_class(item.arch_id).1 != class {
                continue;
            }
            let count = match &item.kind {
                EquipKind::Cargo(c) => c.count.cast_signed(),
                _ => 0,
            };
            return Some(if count > limit { 0 } else { limit - count });
        }
        Some(limit)
    }

    /// `CShip::get_repair_kit_capacity_remaining` (`nanobot_limit`).
    #[must_use]
    pub fn repair_kit_capacity_remaining(&self, h: ObjHandle) -> Option<i32> {
        let limit = self.ship_arch(h)?.nanobot_limit;
        self.kind_capacity_remaining(h, limit, CLASS_REPAIR_KIT)
    }

    /// `CShip::get_shield_battery_capacity_remaining`
    /// (`shield_battery_limit`).
    #[must_use]
    pub fn shield_battery_capacity_remaining(&self, h: ObjHandle) -> Option<i32> {
        let limit = self.ship_arch(h)?.shield_battery_limit;
        self.kind_capacity_remaining(h, limit, CLASS_SHIELD_BATTERY)
    }

    /// `CShip::get_ammo_capacity_remaining(arch)`: `MAX_PLAYER_AMMO` less
    /// the count of the first cargo stack of that archetype (0 above it;
    /// -1 when that stack is no munition, mine or counter-measure, or for
    /// archetype 0); the limit without such a stack.
    #[must_use]
    pub fn ammo_capacity_remaining(&self, h: ObjHandle, arch: u32) -> Option<i32> {
        self.ship_arch(h)?;
        let limit = self.data.constants.general.max_player_ammo;
        if arch == 0 {
            return Some(-1);
        }
        let eq = self.objects.get(h)?.as_equipped()?;
        for slot in eq.equipment.traverse(types::CARGO, false) {
            let Some(item) = eq.equipment.get(slot) else {
                continue;
            };
            if item.arch_id != arch {
                continue;
            }
            let class = self.equip_volume_class(item.arch_id).1;
            if !matches!(class, CLASS_MUNITION | CLASS_MINE | CLASS_COUNTER_MEASURE) {
                return Some(-1);
            }
            let count = match &item.kind {
                EquipKind::Cargo(c) => c.count.cast_signed(),
                _ => 0,
            };
            return Some(if count > limit { 0 } else { limit - count });
        }
        Some(limit)
    }

    /// `CShip::get_space_for_cargo_type(arch)`: how many units of an
    /// equipment archetype the hold takes (`floor` of the room over the
    /// volume; `i32::MAX` for no volume), then the smaller of that and the
    /// kind's own limit for repair kits, shield batteries and ammunition.
    #[must_use]
    pub fn space_for_cargo_type(&self, h: ObjHandle, arch: u32) -> Option<i32> {
        let hold = self.ship_arch(h)?.hold_size;
        let (volume, class) = self.equip_volume_class(arch);
        let mut space = i32::MAX;
        if volume > 0.0 {
            let used = self.hold_used(h)?;
            let remaining = if used <= hold { e(hold) - e(used) } else { 0.0 };
            space = neuron_core::num::trunc_to_i32_via_i64((remaining / e(volume)).floor());
        }
        Some(match class {
            CLASS_REPAIR_KIT => space.min(self.repair_kit_capacity_remaining(h)?),
            CLASS_SHIELD_BATTERY => space.min(self.shield_battery_capacity_remaining(h)?),
            CLASS_MUNITION | CLASS_MINE | CLASS_COUNTER_MEASURE => {
                space.min(self.ammo_capacity_remaining(h, arch)?)
            }
            _ => space,
        })
    }

    /// `GetEquipDesc` of one item (`CEquip`, `CInternalEquip`,
    /// `CExternalEquip`, `CECargo`, `CEShield`): `None` for a destroyed
    /// item, an empty stack or a shield.
    fn scanned_equip(&self, h: ObjHandle, id: SubObjId) -> Option<ScannedEquip> {
        let eq = self.objects.get(h)?.as_equipped()?;
        let item = eq.equipment.by_id(id)?;
        match &item.kind {
            EquipKind::Shield(_) => None,
            EquipKind::Cargo(c) => {
                if c.count == 0 {
                    return None;
                }
                // The health is the stack's hit points over the
                // archetype's, stored to a float; the hardpoint the linked
                // pod's (`GetParentHPName`), else the cargo bay.
                let hardpoint = c
                    .pod
                    .and_then(|p| eq.equipment.by_id(p))
                    .and_then(|p| p.mount.as_ref())
                    .map(|m| m.hardpoint.clone());
                Some(ScannedEquip {
                    id: item.id,
                    arch_id: item.arch_id,
                    hardpoint,
                    equipped: false,
                    health: r(e(c.hit_pts) / e(item.arch.hit_pts)),
                    count: c.count.cast_signed(),
                    mission: item.mission,
                })
            }
            _ => {
                if item.is_destroyed() {
                    return None;
                }
                Some(ScannedEquip {
                    id: item.id,
                    arch_id: item.arch_id,
                    hardpoint: item.mount.as_ref().map(|m| m.hardpoint.clone()),
                    equipped: true,
                    health: r(item.relative_health()),
                    count: 1,
                    mission: item.mission,
                })
            }
        }
    }

    /// `CEScanner::ScanCargo(target)` of the scanner `scanner` of `h`: the
    /// target within the cargo scan range (the larger of the two objects'
    /// scanner interference taken off), the scanner's power threshold
    /// (`power_usage`) drawn from the owner's power, then `GetEquipDesc` of
    /// every item of the target that is not destroyed, both lists in order.
    /// The list is empty unless the answer is [`ScanResult::Complete`].
    pub fn scan_cargo(
        &mut self,
        h: ObjHandle,
        scanner: SubObjId,
        target: Option<ObjHandle>,
    ) -> (ScanResult, Vec<ScannedEquip>) {
        let none = (ScanResult::NoTarget, Vec::new());
        let Some(target) = target.filter(|t| {
            self.objects
                .get(*t)
                .is_some_and(|o| o.as_equipped().is_some())
        }) else {
            return none;
        };
        let (Some(o), Some(t)) = (self.objects.get(h), self.objects.get(target)) else {
            return none;
        };
        let Some((range, threshold)) = o
            .as_equipped()
            .and_then(|eq| eq.equipment.by_id(scanner))
            .and_then(|e| match &e.kind {
                EquipKind::Scanner(s) => Some((s.cargo_scan_range, s.power_usage)),
                _ => None,
            })
        else {
            return none;
        };
        let target_interference = t.simple.scanner_interference;
        let mut interference = e(o.simple.scanner_interference);
        if target_interference > o.simple.scanner_interference {
            interference = e(target_interference);
        }
        let range = (1.0 - interference) * e(range);
        let (tp, op) = (t.engine.position, o.engine.position);
        let dx = e(tp.x) - e(op.x);
        let dy = e(tp.y) - e(op.y);
        let dz = e(tp.z) - e(op.z);
        let dist_sq = (dy * dy + dz * dz) + dx * dx;
        if range * range < dist_sq {
            return (ScanResult::TooFar, Vec::new());
        }
        let power = o.as_equipped().map_or(0.0, |eq| eq.power);
        if !(threshold <= power) {
            return (ScanResult::NotEnoughPower, Vec::new());
        }
        if let Some(eq) = self.objects.get_mut(h).and_then(|o| o.as_equipped_mut()) {
            eq.power = r(e(eq.power) - e(threshold));
        }
        let ids: Vec<SubObjId> = self
            .objects
            .get(target)
            .and_then(|o| o.as_equipped())
            .map(|eq| {
                eq.equipment
                    .traverse(u32::MAX, false)
                    .into_iter()
                    .filter_map(|s| eq.equipment.get(s).map(|e| e.id))
                    .collect()
            })
            .unwrap_or_default();
        let list = ids
            .into_iter()
            .filter_map(|id| self.scanned_equip(target, id))
            .collect();
        (ScanResult::Complete, list)
    }
}
