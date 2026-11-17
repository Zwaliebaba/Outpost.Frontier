//! The shield and its generators (`CppSource/Common/common_ceshield.cpp`).
//!
//! A shield (`CEShield`) is made by the owner's `link_shields` from an
//! archetype `shield_link`; each generator on one of the link's hardpoints
//! joins it (`AddGen`), which sets the shield's capacity, offline
//! threshold and rebuild time. The generators recharge the shield from the
//! owner's power by real elapsed time (`Timing::GetGlobalTime`), which the
//! world passes in as its clock.
//!
//! The intruder checker (a phantom that keeps a shield offline while
//! something sits inside it) is never created in the reference
//! (`CEShield::Destroy`'s note), so it is not ported.

use crate::data::archetype::equipment::ShieldGenerator as GeneratorData;
use crate::data::fuse::WeaponModDb;

use super::{EquipEvent, SubObjId};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `CEShieldGenerator`'s state.
#[derive(Clone, Debug, PartialEq)]
pub struct ShieldGenerator {
    /// `shield_type`.
    pub shield_type: u32,
    /// +0x90 `regeneration_rate`.
    pub regeneration_rate: f32,
    /// `max_capacity`.
    pub max_capacity: f32,
    /// +0x98 `constant_power_draw`.
    pub constant_power_draw: f32,
    /// +0x9c `rebuild_power_draw`.
    pub rebuild_power_draw: f32,
    /// `offline_threshold`.
    pub offline_threshold: f32,
    /// `offline_rebuild_time`.
    pub offline_rebuild_time: f32,
    /// `toughness`.
    pub toughness: i32,
    /// The archetype's nickname (the dominant-generator test).
    pub nickname: u32,
    /// +0x4c the linked shield's id.
    pub linked_shield: Option<SubObjId>,
    /// +0x54 the global time of the last update.
    pub last_update_time: f64,
}

impl ShieldGenerator {
    /// `CEShieldGenerator::CEShieldGenerator`.
    #[must_use]
    pub fn new(data: &GeneratorData) -> Self {
        Self {
            shield_type: data.shield_type,
            regeneration_rate: data.regeneration_rate,
            max_capacity: data.max_capacity,
            constant_power_draw: data.constant_power_draw,
            rebuild_power_draw: data.rebuild_power_draw,
            offline_threshold: data.offline_threshold,
            offline_rebuild_time: data.offline_rebuild_time,
            toughness: data.attached.toughness,
            nickname: 0,
            linked_shield: None,
            last_update_time: 0.0,
        }
    }

    /// `CEShieldGenerator::Update`'s power and regeneration step, given
    /// the elapsed real time: draws the constant power (and the rebuild
    /// power while the shield is not full) when the owner has it, and then
    /// regenerates the shield. Returns the shield's new hit points when it
    /// regenerated.
    #[must_use]
    pub fn regenerate(
        &self,
        functioning: bool,
        real_dt: f32,
        shield_relative_health: f64,
        shield_hit_points: f32,
        owner_power: &mut f32,
    ) -> Option<f32> {
        if !functioning {
            return None;
        }
        let recharging = shield_relative_health < 1.0;
        let mut draw = e(real_dt) * e(self.constant_power_draw);
        if recharging {
            draw += e(real_dt) * e(self.rebuild_power_draw);
        }
        let power = e(*owner_power);
        if draw <= power {
            *owner_power = r(power - draw);
            if recharging {
                return Some(r(e(real_dt) * e(self.regeneration_rate) + e(shield_hit_points)));
            }
        }
        None
    }
}

/// `CEShield`'s state.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Shield {
    /// +0x4c the shield takes hits (`bOnline`).
    pub online: bool,
    /// +0x50 the current hit points.
    pub hit_pts: f32,
    /// +0x58 the global time before which it cannot come back.
    pub offline_end_time: f64,
    /// The linked generators (ids), in link order.
    pub generators: Vec<SubObjId>,
    /// The dominant generator: its nickname and toughness.
    pub dominant: Option<(u32, i32)>,
    /// +0x74 the averaged offline threshold.
    pub offline_threshold: f32,
    /// +0x78 the longest rebuild time.
    pub offline_rebuild_time: f32,
    /// +0x7c the summed capacity.
    pub max_hit_pts: f32,
}

/// What the shield state machine needs from its item.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShieldStatus {
    /// `IsActive()`.
    pub active: bool,
    /// `IsDisabled()`.
    pub disabled: bool,
    /// `IsDestroyed()`.
    pub destroyed: bool,
    /// The item has an attachment (connected).
    pub attached: bool,
}

impl Shield {
    /// `IsFunctioning`.
    #[must_use]
    pub fn is_functioning(&self, status: ShieldStatus) -> bool {
        status.active && !status.disabled && !status.destroyed && self.online
    }

    /// `GetRelativeHealth` (the shield's maximum is its own).
    #[must_use]
    pub fn relative_health(&self) -> f64 {
        if self.max_hit_pts == 0.0 {
            return 0.0;
        }
        e(self.hit_pts) / e(self.max_hit_pts)
    }

    /// `CanActivate`.
    #[must_use]
    pub fn can_activate(&self, now: f64) -> bool {
        if !(now >= self.offline_end_time) {
            return false;
        }
        self.relative_health() > e(self.offline_threshold)
    }

    /// `Activate_Internal`: the collision change of the attachment is the
    /// world's; here the flag and the event.
    pub fn activate_internal(&mut self, on: bool, status: ShieldStatus, events: &mut Vec<EquipEvent>) -> bool {
        if self.online != on && status.attached {
            self.online = on;
            events.push(EquipEvent::ShieldOfflineChanged(on));
        }
        self.online
    }

    /// `CEShield::Activate`'s shield part, after `CEquip::Activate`.
    pub fn activate(&mut self, status: ShieldStatus, now: f64, events: &mut Vec<EquipEvent>) -> bool {
        if status.active {
            if self.can_activate(now) {
                self.activate_internal(true, status, events);
            }
            return true;
        }
        if self.online {
            self.activate_internal(false, status, events);
        }
        false
    }

    /// `CEShield::Update`'s shield part.
    pub fn update(&mut self, status: ShieldStatus, now: f64, events: &mut Vec<EquipEvent>) {
        if !self.is_functioning(status) && status.active && self.can_activate(now) {
            events.push(EquipEvent::ShieldCanReactivate);
            self.activate_internal(true, status, events);
        }
    }

    /// `CEShield::SetHitPoints`: capped at the maximum; a functioning
    /// shield at or below its threshold goes offline for its rebuild time.
    pub fn set_hit_points(&mut self, hit: f32, status: ShieldStatus, now: f64, events: &mut Vec<EquipEvent>) {
        let hit = if self.max_hit_pts < hit { self.max_hit_pts } else { hit };
        self.hit_pts = hit;
        if self.is_functioning(status) {
            if self.relative_health() <= e(self.offline_threshold) {
                events.push(EquipEvent::ShieldWentOffline);
                self.offline_end_time = now + e(self.offline_rebuild_time);
                self.activate_internal(false, status, events);
            }
        } else if status.active && self.can_activate(now) {
            events.push(EquipEvent::ShieldCanReactivate);
            self.activate_internal(true, status, events);
        }
    }

    /// `AddGen`: the generator joins; the threshold is the running mean,
    /// the rebuild time the longest, the capacity the sum, and the shield
    /// gains the generator's capacity.
    pub fn add_gen(
        &mut self,
        id: SubObjId,
        generator: &ShieldGenerator,
        status: ShieldStatus,
        now: f64,
        events: &mut Vec<EquipEvent>,
    ) {
        if self.dominant.is_none_or(|(_, t)| t < generator.toughness) {
            self.dominant = Some((generator.nickname, generator.toughness));
        }
        let count = self.generators.len() as f64;
        self.offline_threshold =
            r((count * e(self.offline_threshold) + e(generator.offline_threshold)) / (count + 1.0));
        if self.offline_rebuild_time < generator.offline_rebuild_time {
            self.offline_rebuild_time = generator.offline_rebuild_time;
        }
        self.max_hit_pts = r(e(generator.max_capacity) + e(self.max_hit_pts));
        let hit = r(e(self.hit_pts) + e(generator.max_capacity));
        self.set_hit_points(hit, status, now, events);
        self.generators.push(id);
        self.update(status, now, events);
    }

    /// `RemGen`: the sums are rebuilt from the remaining generators.
    pub fn rem_gen(
        &mut self,
        id: SubObjId,
        remaining: &[(SubObjId, &ShieldGenerator)],
        status: ShieldStatus,
        now: f64,
        events: &mut Vec<EquipEvent>,
    ) {
        let old = self.dominant;
        self.dominant = None;
        self.offline_threshold = 0.0;
        self.offline_rebuild_time = 0.0;
        self.max_hit_pts = 0.0;
        self.generators.retain(|g| *g != id);
        for &(gid, g) in remaining {
            if !self.generators.contains(&gid) {
                continue;
            }
            if self.dominant.is_none_or(|(_, t)| t < g.toughness) {
                self.dominant = Some((g.nickname, g.toughness));
            }
            self.offline_threshold = r(e(g.offline_threshold) + e(self.offline_threshold));
            if self.offline_rebuild_time < g.offline_rebuild_time {
                self.offline_rebuild_time = g.offline_rebuild_time;
            }
            self.max_hit_pts = r(e(g.max_capacity) + e(self.max_hit_pts));
        }
        if let (Some((now_dominant, _)), Some((was, _))) = (self.dominant, old)
            && now_dominant != was
        {
            events.push(EquipEvent::ShieldGeneratorsChanged);
        }
        let count = self.generators.len();
        if count != 0 {
            self.offline_threshold = r(e(self.offline_threshold) / count as f64);
        }
        if self.max_hit_pts == 0.0 {
            self.set_hit_points(0.0, status, now, events);
        }
        self.update(status, now, events);
    }

    /// `GetWeaponModifier(weapon_type)`: the mean of the generators'
    /// shield modifiers, 1 without generators; unrounded.
    #[must_use]
    pub fn weapon_modifier(&self, weapon_type: u32, shield_types: &[u32], mods: &WeaponModDb) -> f64 {
        if shield_types.is_empty() {
            return 1.0;
        }
        let mut sum = 0.0f32;
        for &t in shield_types {
            sum = r(e(mods.shield_mod(weapon_type, t)) + e(sum));
        }
        e(sum) / shield_types.len() as f64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn generator(capacity: f32, threshold: f32) -> ShieldGenerator {
        ShieldGenerator {
            shield_type: 1,
            regeneration_rate: 10.0,
            max_capacity: capacity,
            constant_power_draw: 1.0,
            rebuild_power_draw: 5.0,
            offline_threshold: threshold,
            offline_rebuild_time: 4.0,
            toughness: 3,
            nickname: 7,
            linked_shield: Some(0xFFF1),
            last_update_time: 0.0,
        }
    }

    const ON: ShieldStatus = ShieldStatus {
        active: true,
        disabled: false,
        destroyed: false,
        attached: true,
    };

    #[test]
    fn generators_fill_the_shield() {
        let mut s = Shield::default();
        let mut ev = Vec::new();
        s.add_gen(0x30, &generator(200.0, 0.2), ON, 0.0, &mut ev);
        assert_eq!(s.max_hit_pts, 200.0);
        assert_eq!(s.hit_pts, 200.0);
        assert!(s.online);
        s.add_gen(0x31, &generator(100.0, 0.4), ON, 0.0, &mut ev);
        assert_eq!(s.max_hit_pts, 300.0);
        assert_eq!(s.offline_threshold, r((0.2f32 as f64 + 0.4f32 as f64) / 2.0));
    }

    #[test]
    fn low_hit_points_take_it_offline_for_the_rebuild_time() {
        let mut s = Shield::default();
        let mut ev = Vec::new();
        s.add_gen(0x30, &generator(200.0, 0.2), ON, 0.0, &mut ev);
        s.set_hit_points(30.0, ON, 10.0, &mut ev);
        assert!(!s.online);
        assert_eq!(s.offline_end_time, 14.0);
        // Above the threshold but before the rebuild time: still offline.
        s.set_hit_points(100.0, ON, 12.0, &mut ev);
        assert!(!s.online);
        s.update(ON, 14.5, &mut ev);
        assert!(s.online);
        assert!(ev.contains(&EquipEvent::ShieldWentOffline));
    }

    #[test]
    fn regeneration_draws_power() {
        let g = generator(200.0, 0.2);
        let mut power = 100.0;
        let hit = g.regenerate(true, 1.0, 0.5, 100.0, &mut power);
        assert_eq!(hit, Some(110.0));
        assert_eq!(power, 94.0);
        let mut starved = 3.0;
        assert_eq!(g.regenerate(true, 1.0, 0.5, 100.0, &mut starved), None);
        assert_eq!(starved, 3.0);
    }
}
