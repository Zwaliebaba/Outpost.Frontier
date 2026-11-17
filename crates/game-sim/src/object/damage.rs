//! Damage: `DamageList` (`CppSource/Common/common_damagelist.cpp`), the
//! collision groups (`CArchGroup`, `common_carchgroup.cpp`), and the
//! damage computation of a hit (`EqObj::munition_hit`, `damage_hull`,
//! `damage_equip`, `damage_shield`, `damage_group`, `damage_power`,
//! `destroy_equip`, `destroy_group`; `Server/server_gameobject.cpp:2931`,
//! `:3322-3780`).
//!
//! The reference fills the damage list in the server's object wrapper and
//! applies it there (P3.3); the computation itself only reads the target's
//! hit points, equipment, collision groups and power, so it lives here as
//! functions over a [`DamageTarget`] view. The damage replay test runs them
//! from the oracle's recorded `munition_hit` inputs.
//!
//! Fuses (`light_fuse` when a sub-object crosses a fuse's threshold) and
//! the loot drops of fates 4 and 5 are the server's and come with P3.3.

use neuron_core::id::sub_object::{is_arch_group_id, is_equip_id};
use neuron_core::scene::InstanceId;

use crate::data::archetype::eq_obj::CollisionGroup;
use crate::equip::launcher::ProjectileArch;
use crate::equip::{
    EquipKind, EquipManager, Fate, SUBOBJ_ID_NONE, SUBOBJ_ID_POWER, SUBOBJ_ID_ROOT, SubObjId, types,
};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `DamageCause` (`include/common_misc.h:259`). Values from 0x10 up bypass
/// invincibility (`is_damage_blocked`).
pub type DamageCause = i32;

/// `DamageCause::Gun`.
pub const CAUSE_GUN: DamageCause = 1;
/// `DamageCause::Missile`.
pub const CAUSE_MISSILE: DamageCause = 2;
/// `DamageCause::Mine`.
pub const CAUSE_MINE: DamageCause = 3;
/// `DamageCause::Collision`.
pub const CAUSE_COLLISION: DamageCause = 4;
/// `DamageCause::Explosion`.
pub const CAUSE_EXPLOSION: DamageCause = 5;

/// `DamageEntry` (0xc bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DamageEntry {
    /// +0x00 the sub-object.
    pub sub_obj_id: SubObjId,
    /// +0x04 its hit points after the damage.
    pub hit_pts: f32,
    /// +0x08 its fate.
    pub fate: Fate,
}

/// `DamageList`: the entries one damage event produced, before they are
/// applied.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DamageList {
    /// The entries, one per sub-object, in insertion order.
    pub entries: Vec<DamageEntry>,
    /// +0x0c the object is destroyed.
    pub destroyed: bool,
    /// +0x10 the cause.
    pub cause: DamageCause,
    /// +0x14 the inflicting object's id.
    pub inflictor_id: u32,
    /// +0x18 the inflicting player.
    pub inflictor_owner: u32,
}

impl DamageList {
    /// A list with its cause and inflictor.
    #[must_use]
    pub fn new(cause: DamageCause, inflictor_id: u32, inflictor_owner: u32) -> Self {
        Self {
            cause,
            inflictor_id,
            inflictor_owner,
            ..Self::default()
        }
    }

    /// `add_damage_entry`: one entry per sub-object; a repeat keeps the
    /// lower hit points (the new value unless the old is strictly less)
    /// and the more severe fate. A root entry at or below 0 destroys.
    pub fn add_damage_entry(&mut self, sub_obj_id: SubObjId, hit_pts: f32, fate: Fate) {
        if let Some(entry) = self.entries.iter_mut().find(|x| x.sub_obj_id == sub_obj_id) {
            entry.hit_pts = if entry.hit_pts < hit_pts {
                entry.hit_pts
            } else {
                hit_pts
            };
            if fate.value() > entry.fate.value() {
                entry.fate = fate;
            }
        } else {
            self.entries.push(DamageEntry {
                sub_obj_id,
                hit_pts,
                fate,
            });
        }
        if sub_obj_id == SUBOBJ_ID_ROOT && hit_pts <= 0.0 {
            self.destroyed = true;
        }
    }

    /// `get_hit_pts_left`: the entry's hit points, -1 without one.
    #[must_use]
    pub fn hit_pts_left(&self, sub_obj_id: SubObjId) -> f32 {
        self.entries
            .iter()
            .find(|x| x.sub_obj_id == sub_obj_id)
            .map_or(-1.0, |x| x.hit_pts)
    }

    /// `Count`.
    #[must_use]
    pub fn count(&self) -> usize {
        self.entries.len()
    }
}

/// A collision group's state (`CArchGroup::State`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GroupState {
    /// 1.
    #[default]
    Alive,
    /// 2: killed, not yet destroyed.
    Killed,
    /// 3: destroyed.
    Destroyed,
}

/// `CArchGroup`: one collision group of an equipped object.
#[derive(Clone, Debug, PartialEq)]
pub struct ArchGroup {
    /// The group's sub-object id.
    pub id: SubObjId,
    /// The group's part name (`obj`).
    pub name: Vec<u8>,
    /// +0x08 its root instance (`FindChild`).
    pub root_instance: Option<InstanceId>,
    /// +0x0c the hit points.
    pub hit_pts: f32,
    /// The archetype's hit points (`GetMaxHitPoints`).
    pub max_hit_pts: i32,
    /// +0x14 the state.
    pub state: GroupState,
    /// +0x18 the fate.
    pub fate: Fate,
    /// The group type.
    pub group_type: u32,
    /// `separable`.
    pub separable: bool,
    /// `destroy_parent`.
    pub destroy_parent: bool,
    /// `root_health_proxy`.
    pub root_health_proxy: bool,
    /// `linked_equip_type`.
    pub linked_equip_type: u32,
    /// `linked_equip_health`.
    pub linked_equip_health: f32,
    /// `explosion_resistance`.
    pub explosion_resistance: f32,
    /// The instances under the group's root (for `IsInstInGroup`), root
    /// first.
    pub instances: Vec<InstanceId>,
}

impl ArchGroup {
    /// `CArchGroup::CArchGroup` from the archetype's group: full health,
    /// alive.
    #[must_use]
    pub fn new(arch: &CollisionGroup, name: Vec<u8>) -> Self {
        Self {
            id: arch.id,
            name,
            root_instance: None,
            hit_pts: arch.hit_pts as f32,
            max_hit_pts: arch.hit_pts,
            state: GroupState::Alive,
            fate: Fate::None,
            group_type: arch.group_type,
            separable: arch.separable,
            destroy_parent: arch.destroy_parent,
            root_health_proxy: arch.root_health_proxy,
            linked_equip_type: arch.linked_equip_type,
            linked_equip_health: arch.linked_equip_health,
            explosion_resistance: arch.explosion_resistance,
            instances: Vec::new(),
        }
    }

    /// `GetMaxHitPoints`, unrounded.
    #[must_use]
    pub fn max_hit_points(&self) -> f64 {
        f64::from(self.max_hit_pts)
    }

    /// `GetRelativeHealth`.
    #[must_use]
    pub fn relative_health(&self) -> f64 {
        e(self.hit_pts) / self.max_hit_points()
    }

    /// `IsInstInGroup`.
    #[must_use]
    pub fn is_inst_in_group(&self, instance: Option<InstanceId>) -> bool {
        instance.is_some_and(|i| self.instances.contains(&i))
    }

    /// `IsDead`/`IsDestroyed`: killed or destroyed.
    #[must_use]
    pub fn is_dead(&self) -> bool {
        self.state != GroupState::Alive
    }
}

/// What the damage computation reads of its target (the server's `EqObj`
/// and its `CEqObj`).
#[derive(Clone, Copy)]
pub struct DamageTarget<'a> {
    /// The hull's hit points.
    pub hit_pts: f32,
    /// The archetype's hit points.
    pub max_hit_pts: f32,
    /// The owning player (0 for an NPC).
    pub owner_player: u32,
    /// The player's difficulty scale when this is a player's current ship.
    pub difficulty_scale: Option<f32>,
    /// `is_damage_blocked`'s answer for this list's inflictor, and the
    /// invincibility timer it floors at (1 − timer).
    pub invincible: Option<f32>,
    /// The equipment.
    pub equipment: &'a EquipManager,
    /// The collision groups.
    pub groups: &'a [ArchGroup],
    /// The power.
    pub power: f32,
    /// The maximum power.
    pub max_power: f32,
    /// The weapons are offline.
    pub weapons_offline: bool,
    /// Single player (the launcher fate exception is multiplayer only).
    pub single_player: bool,
    /// `PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE`.
    pub player_attached_equip_scale: f32,
    /// `PLAYER_COLLISION_GROUP_HIT_PTS_SCALE`.
    pub player_group_scale: f32,
    /// `ShieldEquipConsts::HULL_DAMAGE_FACTOR`.
    pub shield_hull_damage_factor: f32,
    /// A death fuse burns.
    pub death_fuse_burning: bool,
    /// The archetype's `explosion_resistance` (explosion damage on the
    /// hull and the power).
    pub explosion_resistance: f32,
    /// `CEShield::GetWeaponModifier(weapon_type)` of a shield, unrounded.
    pub shield_modifier: &'a dyn Fn(SubObjId, u32) -> f64,
}

impl DamageTarget<'_> {
    fn blocked(&self, list: &DamageList) -> Option<f32> {
        if list.cause >= 0x10 {
            return None;
        }
        self.invincible
    }

    fn armor_scale(&self) -> Option<f32> {
        match &self.equipment.first(types::ARMOR)?.kind {
            EquipKind::Armor(a) => Some(a.hit_pts_scale),
            _ => None,
        }
    }

    fn group(&self, id: SubObjId) -> Option<&ArchGroup> {
        self.groups.iter().find(|g| g.id == id)
    }
}

/// `GameObject::damage_hull` with `EqObj::damage_hull`'s scaling (the
/// difficulty scale, then the armor). True when the hull is destroyed.
pub fn damage_hull(t: &DamageTarget<'_>, amount: f32, list: &mut DamageList) -> bool {
    let mut amount = amount;
    if amount > 0.0 {
        if let Some(scale) = t.difficulty_scale {
            amount = r(e(scale) * e(amount));
        }
        if let Some(armor) = t.armor_scale() {
            amount = r(e(amount) / e(armor));
        }
    }
    if amount == 0.0 {
        return false;
    }
    let mut current = list.hit_pts_left(SUBOBJ_ID_ROOT);
    if current < 0.0 {
        current = t.hit_pts;
    }
    let mut new = current;
    if amount > 0.0 {
        let left = r(e(current) - e(amount));
        new = if left > 0.0 { left } else { 0.0 };
    } else if amount < 0.0 {
        new = 0.0;
    }
    if let Some(timer) = t.blocked(list) {
        let max = t.max_hit_pts;
        let floor = r(1.0 - e(timer));
        let inv = 1.0 / e(max);
        if e(current) * inv < e(floor) {
            new = current;
        } else if e(new) * inv < e(floor) {
            new = r(e(floor) * e(max));
        }
    }
    if new == current {
        return false;
    }
    list.add_damage_entry(SUBOBJ_ID_ROOT, new, Fate::None);
    if new <= 0.0 {
        list.destroyed = true;
        return true;
    }
    false
}

/// `EqObj::damage_equip`. True when the item is destroyed.
pub fn damage_equip(
    t: &DamageTarget<'_>,
    id: SubObjId,
    damage: f32,
    list: &mut DamageList,
) -> bool {
    if damage == 0.0 {
        return false;
    }
    let Some(equip) = t.equipment.by_id(id) else {
        return false;
    };
    if equip.is_destroyed() {
        return false;
    }
    let mut current = list.hit_pts_left(id);
    if current < 0.0 {
        current = equip.hit_points();
    }
    let mut new = current;
    if damage > 0.0 {
        let mut damage = damage;
        let mut applied = e(damage);
        if equip.type_bits & types::ATTACHED != 0 && t.owner_player != 0 {
            damage = r(e(damage) / e(t.player_attached_equip_scale));
            applied = e(damage);
            if let Some(scale) = t.difficulty_scale {
                applied = e(scale) * e(damage);
            }
        }
        let left = e(current) - applied;
        new = if left > 0.0 { r(left) } else { 0.0 };
    } else if damage < 0.0 {
        new = 0.0;
    }
    if let Some(timer) = t.blocked(list) {
        let floor = r(1.0 - e(timer));
        let max = equip.max_hit_points();
        let rel_current = if max != 0.0 { e(current) / e(max) } else { 0.0 };
        if rel_current < e(floor) {
            new = current;
        } else if e(new) / e(max) < e(floor) {
            new = r(e(max) * e(floor));
        }
    }
    if new == current {
        return false;
    }
    if new > 0.0 {
        list.add_damage_entry(id, new, Fate::None);
        return false;
    }
    let mut fate = Fate::Separated;
    if !t.single_player && t.weapons_offline && equip.type_bits & types::LAUNCHER != 0 {
        fate = Fate::None;
    }
    destroy_equip(t, id, fate, list);
    true
}

/// `EqObj::destroy_equip`: a zero-hit-point entry with the fate. Cargo
/// pods (`destroy_cargo`) and the loot fates 4 and 5 are the server's
/// (P3.3); a loot fate on an item that cannot be looted separates it.
pub fn destroy_equip(t: &DamageTarget<'_>, id: SubObjId, fate: Fate, list: &mut DamageList) {
    let Some(equip) = t.equipment.by_id(id) else {
        return;
    };
    if equip.is_destroyed() {
        return;
    }
    let mut fate = fate;
    if matches!(fate, Fate::Loot | Fate::LootSilent)
        && (!equip.lootable || equip.is_disabled(t.weapons_offline))
    {
        fate = Fate::Separated;
    }
    list.add_damage_entry(id, 0.0, fate);
}

/// `EqObj::damage_shield`. True when the change crosses the shield's
/// offline threshold.
pub fn damage_shield(
    t: &DamageTarget<'_>,
    id: SubObjId,
    damage: f32,
    list: &mut DamageList,
) -> bool {
    if damage == 0.0 {
        return false;
    }
    let Some(equip) = t.equipment.by_id(id) else {
        return false;
    };
    let EquipKind::Shield(shield) = &equip.kind else {
        return false;
    };
    if equip.is_destroyed() {
        return false;
    }
    let mut current = list.hit_pts_left(id);
    if current < 0.0 {
        current = shield.hit_pts;
    }
    let mut new = current;
    if damage > 0.0 {
        let applied = match t.difficulty_scale {
            Some(scale) => e(scale) * e(damage),
            None => e(damage),
        };
        let left = e(current) - applied;
        new = if left > 0.0 { r(left) } else { 0.0 };
    } else if damage < 0.0 {
        new = 0.0;
    }
    if let Some(timer) = t.blocked(list) {
        if !(timer > 0.0) {
            new = current;
        } else {
            let max = shield.max_hit_pts;
            let rel_new = if max != 0.0 { e(new) / e(max) } else { 0.0 };
            let rel_current = if max != 0.0 {
                r(e(current) / e(max))
            } else {
                0.0
            };
            let floor = r(1.0 - e(timer));
            if rel_new < e(floor) {
                if rel_current < floor {
                    new = current;
                } else {
                    new = r(e(max) * e(floor));
                }
            }
        }
    }
    if new == current {
        return false;
    }
    list.add_damage_entry(id, new, Fate::None);
    let max = shield.max_hit_pts;
    let rel_new = if max != 0.0 { r(e(new) / e(max)) } else { 0.0 };
    let threshold = shield.offline_threshold;
    let rel_old = shield.relative_health();
    if rel_old > e(threshold) {
        return rel_new < threshold;
    }
    rel_new > threshold
}

/// `EqObj::damage_group`. True when the group (or, through a health
/// proxy, the hull) is destroyed.
pub fn damage_group(
    t: &DamageTarget<'_>,
    id: SubObjId,
    damage: f32,
    list: &mut DamageList,
    in_group: &dyn Fn(&ArchGroup, Option<InstanceId>) -> bool,
    mount_of: &dyn Fn(SubObjId) -> Option<InstanceId>,
) -> bool {
    if damage == 0.0 {
        return false;
    }
    let Some(group) = t.group(id) else {
        return false;
    };
    if group.is_dead() {
        return false;
    }
    if !(group.hit_pts > 0.0) && !group.hit_pts.is_nan() {
        return false;
    }
    let mut current = list.hit_pts_left(id);
    if current < 0.0 {
        current = group.hit_pts;
    }
    let mut new = current;
    if damage > 0.0 {
        let mut scaled = e(damage);
        if let Some(armor) = t.armor_scale() {
            scaled = e(damage) / e(armor);
        }
        let mut applied = scaled;
        if t.owner_player != 0 {
            let owned = r(scaled / e(t.player_group_scale));
            applied = e(owned);
            if let Some(scale) = t.difficulty_scale {
                applied = e(scale) * e(owned);
            }
        }
        let left = e(current) - applied;
        new = if left > 0.0 { r(left) } else { 0.0 };
    } else if damage < 0.0 {
        new = 0.0;
    }
    if let Some(timer) = t.blocked(list) {
        let floor = r(1.0 - e(timer));
        let max_exact = group.max_hit_points();
        let max = r(max_exact);
        let rel_current = if max_exact != 0.0 {
            e(current) / e(max)
        } else {
            0.0
        };
        if rel_current < e(floor) {
            new = current;
        } else {
            let rel_new = if max != 0.0 { e(new) / e(max) } else { 0.0 };
            if rel_new < e(floor) {
                new = r(e(max) * e(floor));
            }
        }
    }
    let mut destroyed = false;
    if new != current {
        let fuse_holds = t.death_fuse_burning && list.cause < 0x10;
        if !(new > 0.0) && !fuse_holds {
            destroy_group(t, id, Fate::Separated, list, true, in_group, mount_of);
            destroyed = true;
        } else {
            list.add_damage_entry(id, new, Fate::None);
            destroyed = new <= 0.0;
        }
    }
    if group.root_health_proxy && damage_hull(t, damage, list) {
        return true;
    }
    destroyed
}

/// `EqObj::destroy_group`: a group that takes its parent marks the list
/// destroyed; otherwise its linked equipment takes damage, its contents
/// (equipment mounted in it, groups under it) are destroyed when asked,
/// and the group gets a zero entry with the fate.
pub fn destroy_group(
    t: &DamageTarget<'_>,
    id: SubObjId,
    fate: Fate,
    list: &mut DamageList,
    with_children: bool,
    in_group: &dyn Fn(&ArchGroup, Option<InstanceId>) -> bool,
    mount_of: &dyn Fn(SubObjId) -> Option<InstanceId>,
) {
    let Some(group) = t.group(id) else {
        return;
    };
    if group.is_dead() {
        return;
    }
    let hit = group.hit_pts;
    if !(hit > 0.0) && !hit.is_nan() && group.fate == fate {
        return;
    }
    if group.destroy_parent {
        list.destroyed = true;
        return;
    }
    damage_linked_equipment(t, group, list);
    if with_children {
        for slot in t.equipment.traverse(types::EXTERNAL, false) {
            let Some(equip) = t.equipment.get(slot) else {
                continue;
            };
            if !in_group(group, mount_of(equip.id)) {
                continue;
            }
            if equip.type_bits == types::LIGHT || equip.type_bits == types::ATTACHED_FX {
                continue;
            }
            destroy_equip(t, equip.id, Fate::Destroyed, list);
        }
        for child in t.groups {
            if child.id == group.id || !in_group(group, child.root_instance) {
                continue;
            }
            destroy_group(
                t,
                child.id,
                Fate::Destroyed,
                list,
                false,
                in_group,
                mount_of,
            );
        }
    }
    list.add_damage_entry(group.id, 0.0, fate);
}

/// `EqObj::damage_linked_equipment`: every item of the group's linked type
/// takes the linked share of its maximum (linked shields are an
/// unimplemented branch in the reference: it reports and returns).
pub fn damage_linked_equipment(t: &DamageTarget<'_>, group: &ArchGroup, list: &mut DamageList) {
    let linked = group.linked_equip_type;
    if linked == 0 || linked == types::SHIELD {
        return;
    }
    for slot in t.equipment.traverse(linked, false) {
        if let Some(equip) = t.equipment.get(slot) {
            let amount = r(e(group.linked_equip_health) * e(equip.max_hit_points()));
            damage_equip(t, equip.id, amount, list);
        }
    }
}

/// `EqObj::damage_power`: energy damage on the power (no power plant, no
/// damage). True when the power is at or below 0.
pub fn damage_power(t: &DamageTarget<'_>, damage: f32, list: &mut DamageList) -> bool {
    if damage == 0.0 || t.max_power <= 0.0 {
        return false;
    }
    let mut current = list.hit_pts_left(SUBOBJ_ID_POWER);
    if current < 0.0 {
        current = t.power;
    }
    let mut new = current;
    if damage > 0.0 {
        let applied = match t.difficulty_scale {
            Some(scale) => e(scale) * e(damage),
            None => e(damage),
        };
        let left = e(current) - applied;
        new = if left > 0.0 { r(left) } else { 0.0 };
    } else if damage < 0.0 {
        new = 0.0;
    }
    if let Some(timer) = t.blocked(list) {
        let max = t.max_power;
        let floor = r(1.0 - e(timer));
        let inv = 1.0 / e(max);
        if e(current) * inv < e(floor) {
            new = current;
        } else if e(new) * inv < e(floor) {
            new = r(e(floor) * e(max));
        }
    }
    if new == current {
        return false;
    }
    list.add_damage_entry(SUBOBJ_ID_POWER, new, Fate::None);
    new <= 0.0
}

/// `EqObj::munition_hit`: the hull for the root (or no sub-object), the
/// group or the item that was hit; a shield takes the whole hit (hull
/// damage times the shield factor plus energy, times the weapon
/// modifier); otherwise the energy damage drains the power.
pub fn munition_hit(
    t: &DamageTarget<'_>,
    munition: &ProjectileArch,
    sub_obj_id: SubObjId,
    list: &mut DamageList,
    in_group: &dyn Fn(&ArchGroup, Option<InstanceId>) -> bool,
    mount_of: &dyn Fn(SubObjId) -> Option<InstanceId>,
) {
    if sub_obj_id == SUBOBJ_ID_ROOT || sub_obj_id == SUBOBJ_ID_NONE {
        damage_hull(t, munition.hull_damage, list);
    } else if is_arch_group_id(sub_obj_id) {
        if t.group(sub_obj_id).is_none() {
            return;
        }
        damage_group(
            t,
            sub_obj_id,
            munition.hull_damage,
            list,
            in_group,
            mount_of,
        );
    } else if is_equip_id(sub_obj_id) {
        let Some(equip) = t.equipment.by_id(sub_obj_id) else {
            return;
        };
        if equip.type_bits == types::SHIELD {
            let total = r(e(munition.hull_damage) * e(t.shield_hull_damage_factor)
                + e(munition.energy_damage));
            let modifier = (t.shield_modifier)(sub_obj_id, munition.weapon_type);
            damage_shield(t, sub_obj_id, r(modifier * e(total)), list);
            return;
        }
        damage_equip(t, sub_obj_id, munition.hull_damage, list);
    } else {
        return;
    }
    damage_power(t, munition.energy_damage, list);
}

/// An explosion's damage event (`ExplosionDamageEvent`): where, and the
/// archetype's radius and damage.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Blast {
    /// The centre.
    pub position: neuron_core::math::Vector,
    /// `radius`.
    pub radius: f32,
    /// `hull_damage`.
    pub hull_damage: f32,
    /// `energy_damage`.
    pub energy_damage: f32,
}

/// The spheres the explosion passes measure against: the hull's
/// explosion-damage sphere (`get_explosion_dmg_bounding_sphere`), the
/// physical sphere (`get_physical_radius_r`), and each attached item's and
/// collision group's (`GetRadius`, `GetCenterOfMass`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BlastSpheres {
    /// The hull's sphere.
    pub hull: (f32, neuron_core::math::Vector),
    /// The physical sphere.
    pub physical: (f32, neuron_core::math::Vector),
    /// The attached items' spheres.
    pub equipment: Vec<(SubObjId, f32, neuron_core::math::Vector)>,
    /// The collision groups' spheres.
    pub groups: Vec<(SubObjId, f32, neuron_core::math::Vector)>,
}

/// `EqObj_ExplosionBand`: 3 within a ninth of the radius squared, 2 within
/// four ninths, 1 within the radius, else 0; `distance` is the squared
/// centre distance less the target's squared radius, floored at 0.
#[must_use]
pub fn explosion_band(distance: f64, radius_sq: f32, middle: f32, inner: f32) -> i32 {
    let distance = if distance > 0.0 { distance } else { 0.0 };
    if distance < e(inner) {
        3
    } else if distance < e(middle) {
        2
    } else if distance < e(radius_sq) {
        1
    } else {
        0
    }
}

fn bands(blast: &Blast) -> (f32, f32, f32) {
    let radius_sq = r(e(blast.radius) * e(blast.radius));
    let middle = r(e(radius_sq) * e(4.0f32 / 9.0));
    let inner = r(e(radius_sq) * e(1.0f32 / 9.0));
    (radius_sq, middle, inner)
}

fn sphere_distance(
    p: neuron_core::math::Vector,
    radius: f32,
    center: neuron_core::math::Vector,
    order_xz: bool,
) -> f64 {
    let dx = e(p.x) - e(center.x);
    let dy = e(p.y) - e(center.y);
    let dz = e(p.z) - e(center.z);
    let sum = if order_xz {
        (dz * dz + dx * dx) + dy * dy
    } else {
        (dz * dz + dy * dy) + dx * dx
    };
    sum - e(radius) * e(radius)
}

/// `EqObj::explosion_hit`: a functioning shield absorbs the whole blast
/// (`explosion_hit_shield`); otherwise the hull, the attached items and
/// collision groups, and the power take their bands' share.
pub fn explosion_hit(
    t: &DamageTarget<'_>,
    blast: &Blast,
    spheres: &BlastSpheres,
    list: &mut DamageList,
    in_group: &dyn Fn(&ArchGroup, Option<InstanceId>) -> bool,
    mount_of: &dyn Fn(SubObjId) -> Option<InstanceId>,
) {
    let (radius_sq, middle, inner) = bands(blast);
    let third = e(1.0f32 / 3.0);
    // `explosion_hit_shield`.
    let mut shielded = false;
    for slot in t.equipment.traverse(types::SHIELD, false) {
        let Some(equip) = t.equipment.get(slot) else {
            continue;
        };
        let EquipKind::Shield(s) = &equip.kind else {
            continue;
        };
        let status = crate::equip::shield::ShieldStatus {
            active: equip.is_active(),
            disabled: false,
            destroyed: equip.is_destroyed(),
            attached: true,
        };
        if !s.is_functioning(status) {
            continue;
        }
        shielded = true;
        let Some(&(_, radius, center)) =
            spheres.equipment.iter().find(|(id, _, _)| *id == equip.id)
        else {
            continue;
        };
        let band = explosion_band(
            sphere_distance(blast.position, radius, center, true),
            radius_sq,
            middle,
            inner,
        );
        if band == 0 {
            continue;
        }
        let amount = r((e(blast.hull_damage) * e(t.shield_hull_damage_factor)
            + e(blast.energy_damage))
            * (f64::from(band) * third));
        damage_shield(
            t,
            equip.id,
            r(e(equip.arch.explosion_resistance) * e(amount)),
            list,
        );
    }
    if shielded {
        return;
    }
    // `explosion_hit_hull`.
    let (radius, center) = spheres.hull;
    let band = explosion_band(
        sphere_distance(blast.position, radius, center, true),
        radius_sq,
        middle,
        inner,
    );
    if band != 0 {
        let amount = r(f64::from(band) * third * e(blast.hull_damage));
        damage_hull(t, r(e(t.explosion_resistance) * e(amount)), list);
    }
    // `explosion_hit_subobjs`: the attached items (mask 0x1ee0), then the
    // groups.
    for slot in t.equipment.traverse(0x1ee0, false) {
        let Some(equip) = t.equipment.get(slot) else {
            continue;
        };
        let Some(&(_, radius, center)) =
            spheres.equipment.iter().find(|(id, _, _)| *id == equip.id)
        else {
            continue;
        };
        let band = explosion_band(
            sphere_distance(blast.position, radius, center, false),
            radius_sq,
            middle,
            inner,
        );
        if band == 0 {
            continue;
        }
        let amount = r(f64::from(band) * third * e(blast.hull_damage));
        damage_equip(
            t,
            equip.id,
            r(e(equip.arch.explosion_resistance) * e(amount)),
            list,
        );
    }
    for group in t.groups {
        let Some(&(_, radius, center)) = spheres.groups.iter().find(|(id, _, _)| *id == group.id)
        else {
            continue;
        };
        let band = explosion_band(
            sphere_distance(blast.position, radius, center, false),
            radius_sq,
            middle,
            inner,
        );
        if band == 0 {
            continue;
        }
        let amount = r(f64::from(band) * third * e(blast.hull_damage));
        damage_group(
            t,
            group.id,
            r(e(group.explosion_resistance) * e(amount)),
            list,
            in_group,
            mount_of,
        );
    }
    // `explosion_hit_power`.
    if t.max_power < 0.0 || blast.energy_damage == 0.0 {
        return;
    }
    let (radius, center) = spheres.physical;
    let band = explosion_band(
        sphere_distance(blast.position, radius, center, true),
        radius_sq,
        middle,
        inner,
    );
    if band == 0 {
        return;
    }
    damage_power(
        t,
        r(f64::from(band) * third * e(blast.energy_damage) * e(t.explosion_resistance)),
        list,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn target<'a>(
        equipment: &'a EquipManager,
        groups: &'a [ArchGroup],
        f: &'a dyn Fn(SubObjId, u32) -> f64,
    ) -> DamageTarget<'a> {
        DamageTarget {
            hit_pts: 100.0,
            max_hit_pts: 100.0,
            owner_player: 0,
            difficulty_scale: None,
            invincible: None,
            equipment,
            groups,
            power: 50.0,
            max_power: 50.0,
            weapons_offline: false,
            single_player: false,
            player_attached_equip_scale: 1.0,
            player_group_scale: 1.0,
            shield_hull_damage_factor: 0.5,
            death_fuse_burning: false,
            explosion_resistance: 1.0,
            shield_modifier: f,
        }
    }

    fn munition(hull: f32, energy: f32) -> ProjectileArch {
        ProjectileArch {
            hull_damage: hull,
            energy_damage: energy,
            ..ProjectileArch::default()
        }
    }

    #[test]
    fn entries_merge_by_sub_object() {
        let mut l = DamageList::default();
        l.add_damage_entry(0x22, 5.0, Fate::None);
        l.add_damage_entry(0x22, 7.0, Fate::Separated);
        assert_eq!(l.count(), 1);
        // The old value stays when it is strictly less.
        assert_eq!(l.entries[0].hit_pts, 5.0);
        assert_eq!(l.entries[0].fate, Fate::Separated);
        l.add_damage_entry(0x22, 3.0, Fate::Destroyed);
        assert_eq!(l.entries[0].hit_pts, 3.0);
        assert_eq!(l.entries[0].fate, Fate::Separated);
        assert_eq!(l.hit_pts_left(9), -1.0);
        l.add_damage_entry(SUBOBJ_ID_ROOT, 0.0, Fate::None);
        assert!(l.destroyed);
    }

    #[test]
    fn hull_hit_drains_hull_and_power() {
        let m = EquipManager::default();
        let f = |_, _| 1.0;
        let t = target(&m, &[], &f);
        let mut l = DamageList::new(CAUSE_GUN, 7, 0);
        munition_hit(
            &t,
            &munition(30.0, 20.0),
            SUBOBJ_ID_ROOT,
            &mut l,
            &|_, _| false,
            &|_| None,
        );
        assert_eq!(
            l.entries,
            vec![
                DamageEntry {
                    sub_obj_id: SUBOBJ_ID_ROOT,
                    hit_pts: 70.0,
                    fate: Fate::None
                },
                DamageEntry {
                    sub_obj_id: SUBOBJ_ID_POWER,
                    hit_pts: 30.0,
                    fate: Fate::None
                },
            ]
        );
        assert!(!l.destroyed);
        munition_hit(
            &t,
            &munition(80.0, 0.0),
            SUBOBJ_ID_ROOT,
            &mut l,
            &|_, _| false,
            &|_| None,
        );
        assert!(l.destroyed);
        assert_eq!(l.hit_pts_left(SUBOBJ_ID_ROOT), 0.0);
    }

    #[test]
    fn blasts_fall_off_in_thirds() {
        assert_eq!(explosion_band(0.0, 900.0, 400.0, 100.0), 3);
        assert_eq!(explosion_band(200.0, 900.0, 400.0, 100.0), 2);
        assert_eq!(explosion_band(800.0, 900.0, 400.0, 100.0), 1);
        assert_eq!(explosion_band(900.0, 900.0, 400.0, 100.0), 0);
        let m = EquipManager::default();
        let f = |_, _| 1.0;
        let t = target(&m, &[], &f);
        let blast = Blast {
            position: neuron_core::math::Vector::ZERO,
            radius: 30.0,
            hull_damage: 90.0,
            energy_damage: 30.0,
        };
        let spheres = BlastSpheres {
            hull: (5.0, neuron_core::math::Vector::ZERO),
            physical: (5.0, neuron_core::math::Vector::ZERO),
            ..BlastSpheres::default()
        };
        let mut l = DamageList::new(CAUSE_EXPLOSION, 7, 0);
        explosion_hit(&t, &blast, &spheres, &mut l, &|_, _| false, &|_| None);
        assert_eq!(l.hit_pts_left(SUBOBJ_ID_ROOT), 10.0);
        assert_eq!(l.hit_pts_left(SUBOBJ_ID_POWER), 20.0);
    }

    #[test]
    fn invincibility_floors_the_hull() {
        let m = EquipManager::default();
        let f = |_, _| 1.0;
        let mut t = target(&m, &[], &f);
        t.invincible = Some(0.5);
        let mut l = DamageList::new(CAUSE_GUN, 7, 0);
        damage_hull(&t, 80.0, &mut l);
        assert_eq!(l.hit_pts_left(SUBOBJ_ID_ROOT), 50.0);
        let mut bypass = DamageList::new(0x10, 7, 0);
        damage_hull(&t, 80.0, &mut bypass);
        assert_eq!(bypass.hit_pts_left(SUBOBJ_ID_ROOT), 20.0);
    }

    #[test]
    fn group_destruction_takes_contents() {
        let mut arch = CollisionGroup::new(3);
        arch.hit_pts = 10;
        let mut child = CollisionGroup::new(4);
        child.hit_pts = 10;
        let groups = vec![
            ArchGroup::new(&arch, b"a".to_vec()),
            ArchGroup::new(&child, b"b".to_vec()),
        ];
        let m = EquipManager::default();
        let f = |_, _| 1.0;
        let t = target(&m, &groups, &f);
        let mut l = DamageList::new(CAUSE_GUN, 7, 0);
        let in_group = |g: &ArchGroup, _i: Option<InstanceId>| g.id == 3;
        munition_hit(&t, &munition(25.0, 0.0), 3, &mut l, &in_group, &|_| None);
        assert_eq!(l.hit_pts_left(3), 0.0);
        assert_eq!(
            l.entries.iter().find(|x| x.sub_obj_id == 3).map(|x| x.fate),
            Some(Fate::Separated)
        );
        assert_eq!(
            l.entries.iter().find(|x| x.sub_obj_id == 4).map(|x| x.fate),
            Some(Fate::Destroyed)
        );
    }
}
