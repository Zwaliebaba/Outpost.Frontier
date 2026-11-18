//! AI fire control (`Common/common_aifirecontrol.cpp`): whether a gun or
//! launcher may fire at a target (state, range, line of fire), where it
//! aims (the lead position with the personality's accuracy spread), and
//! the per-tick decisions of the guns, the missile launchers, the mine
//! droppers and the auto turrets, issued through `IObjAffect` (slot 13
//! `fire`, slot 17 the gun's target).

use neuron_core::math::{Transform, Vector, inverse_transform, transform_xform};

use super::fp::{column2, dot_zyx, e, inline_fast_length, neg, normalize3, r, sub};
use super::gun_groups::{GunGroups, GunSlot, kind};
use super::personality::Personality;
use super::steer::random_vector;
use super::target_manager::TargetList;
use super::timer_table::{TimerTable, slot};
use super::world::{AiCommand, AiRand, AiWorld, EQUIP_ACTIVE, ObjectView, class};

/// The angle `is_pointing_at` is asked with for guns and turrets (3°).
pub const GUN_POINTING_ANGLE: f32 = 0.052_359_88;

/// What `CanFireLauncherAtTarget` answers.
pub mod can_fire {
    /// The item is inactive or not a launcher.
    pub const INACTIVE: i32 = 0;
    /// It may fire.
    pub const OK: i32 = 1;
    /// No target, or the item's state forbids it.
    pub const NOT_READY: i32 = 2;
    /// `EquipFlags` 0x8.
    pub const FLAG_8: i32 = 3;
    /// `EquipFlags` 0x4.
    pub const FLAG_4: i32 = 4;
    /// Out of range.
    pub const OUT_OF_RANGE: i32 = 5;
    /// Something blocks the line of fire.
    pub const BLOCKED: i32 = 7;
    /// `UpdateGunFireState`: the selected preference has no gun bit.
    pub const NO_GUNS: i32 = 8;
}

/// The core of `ComputeLeadFirePosition` once the lead position is known:
/// one `rand()` for the spread's size (`pow(unit, power) * spread_angle`)
/// and, unless the target is a player moving slower than √10, three more
/// for the spread's direction (`RandomVector(1)`); a spread above 0.01
/// moves the aim point off the line from the ship by `|dir| *
/// tan(spread)` along `normalize(dir × random)`.
#[allow(clippy::too_many_arguments)]
pub fn lead_fire_offset(
    lead: Vector,
    own_center: Vector,
    target_is_player: bool,
    target_velocity: Vector,
    power_player: f32,
    power_npc: f32,
    spread_angle: f32,
    rand: &mut impl AiRand,
) -> Vector {
    let dir = sub(lead, own_center);
    let draw = rand.rand();
    let frac = r(f64::from(draw) * e(1.0 / 32768.0));
    let power = if target_is_player {
        let v = target_velocity;
        let speed_sq = (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z);
        if speed_sq < 10.0 {
            return lead;
        }
        power_player
    } else {
        power_npc
    };
    let spread = r(e(frac).powf(e(power)) * e(spread_angle));
    let random = random_vector(rand, 1.0);
    if !(spread > 0.01) {
        return lead;
    }
    let cross = Vector::new(
        r(e(dir.z) * e(random.y) - e(random.z) * e(dir.y)),
        r(e(random.z) * e(dir.x) - e(dir.z) * e(random.x)),
        r(e(dir.y) * e(random.x) - e(random.y) * e(dir.x)),
    );
    let normal = normalize3(cross);
    let length = ((e(dir.z) * e(dir.z) + e(dir.y) * e(dir.y)) + e(dir.x) * e(dir.x)).sqrt();
    let dist = length * e(spread).tan();
    let off_y = r(e(normal.y) * dist);
    let off_z = r(e(normal.z) * dist);
    Vector::new(
        r(e(normal.x) * dist + e(lead.x)),
        r(e(off_y) + e(lead.y)),
        r(e(off_z) + e(lead.z)),
    )
}

/// The world, the ship and what fire control reads of the system manager.
pub struct FireEnv<'a, W: AiWorld + AiRand + ?Sized> {
    /// The world.
    pub world: &'a mut W,
    /// The ship.
    pub ship: W::Handle,
    /// The personality.
    pub personality: &'a Personality,
    /// The timer table (fire guns, drop mines, fire missiles).
    pub timers: &'a TimerTable,
    /// The target manager's attack state.
    pub attack_state: i32,
    /// The target manager's selection priority (the preference's weapon
    /// bits: 1 guns, 2 seekers, 4 dumb missiles, 8 type 0xa).
    pub selection_priority: i32,
}

impl<W: AiWorld + AiRand + ?Sized> FireEnv<'_, W> {
    fn view_of(&self, id: Option<u32>) -> Option<(W::Handle, ObjectView)> {
        let h = self.world.handle_of(id?)?;
        Some((h, self.world.inspect(h)?))
    }

    fn fire(&mut self, equipment: Vec<u16>, target: Option<Vector>) {
        self.world
            .command(self.ship, AiCommand::FireGuns { equipment, target });
    }

    fn aim_gun(&mut self, id: u16, target: Option<u32>) {
        let target = target.and_then(|t| self.world.handle_of(t));
        self.world.command(
            self.ship,
            AiCommand::SetGunTarget {
                equipment: id,
                target,
                subtarget: 0,
            },
        );
    }
}

impl GunGroups {
    /// `ComputeLeadFirePosition(target, direct, spread, sub)`: with
    /// `direct`, the ship's lead position (for the sub-target when one is
    /// given, else its own; the target's position when it has none),
    /// otherwise the target's position; then [`lead_fire_offset`].
    pub fn lead_fire_position<W: AiWorld + AiRand + ?Sized>(
        env: &mut FireEnv<'_, W>,
        target: &ObjectView,
        direct: bool,
        spread_angle: f32,
        subtarget: Option<u16>,
    ) -> Vector {
        let mut lead = target.position;
        if direct {
            match subtarget {
                Some(sub) => {
                    if let Some(p) = env.world.lead_fire_position(env.ship, Some(sub)) {
                        lead = p;
                    }
                }
                None => {
                    let own_is_ship = env
                        .world
                        .inspect(env.ship)
                        .is_some_and(|v| v.is(class::SHIP));
                    lead = own_is_ship
                        .then(|| env.world.lead_fire_position(env.ship, None))
                        .flatten()
                        .unwrap_or(target.position);
                }
            }
        }
        let own_center = env
            .world
            .inspect(env.ship)
            .map_or(Vector::ZERO, |v| v.center);
        let gun = &env.personality.gun;
        lead_fire_offset(
            lead,
            own_center,
            target.is_player,
            target.velocity,
            gun.fire_accuracy_power,
            gun.fire_accuracy_power_npc,
            spread_angle,
            &mut &mut *env.world,
        )
    }

    /// `IsLineOfFireBlocked(target, id)`: the cached answer for the pair,
    /// recomputed once a second from a ray of 20 to 2000 m along the
    /// barrel: blocked when a ship or solar other than the target is hit
    /// nearer than the target. The first query only claims a cache slot.
    pub fn is_line_of_fire_blocked<W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        target: &ObjectView,
        id: u16,
    ) -> bool {
        let mut free = None;
        let mut found = None;
        for (i, s) in self.line_of_fire.iter().enumerate() {
            if !s.in_use {
                free.get_or_insert(i);
                continue;
            }
            if s.target_id == target.id && s.hardpoint == id {
                found = Some(i);
                break;
            }
        }
        let Some(index) = found else {
            if let Some(i) = free {
                self.line_of_fire[i] = super::gun_groups::LineOfFireSlot {
                    in_use: true,
                    target_id: target.id,
                    hardpoint: id,
                    ..super::gun_groups::LineOfFireSlot::default()
                };
            }
            return false;
        };
        let slot = &mut self.line_of_fire[index];
        if !slot.needs_refresh {
            return slot.blocked;
        }
        slot.needs_refresh = false;
        slot.blocked = false;
        slot.age = 0.0;
        let Some(info) = world.equipment(ship, id) else {
            return false;
        };
        let Some((barrel, dir)) = info.barrel else {
            return false;
        };
        if info.flags & (super::world::EQUIP_DESTROYED | super::world::EQUIP_DISABLED) != 0 {
            return false;
        }
        let system = world.system_of(ship).unwrap_or(0);
        let tp = target.position;
        let far_dz = r(e(dir.z) * e(2000.0));
        let far = Vector::new(
            r(e(dir.x) * e(2000.0) + e(barrel.x)),
            r(e(barrel.y) + e(dir.y) * e(2000.0)),
            r(e(barrel.z) + e(far_dz)),
        );
        let near_dz = r(e(dir.z) * e(20.0));
        let near = Vector::new(
            r(e(dir.x) * e(20.0) + e(barrel.x)),
            r(e(dir.y) * e(20.0) + e(barrel.y)),
            r(e(near_dz) + e(barrel.z)),
        );
        for hit in world.find_ray_collisions(system, near, far, 8) {
            let Some(hv) = world.handle_of(hit.object).and_then(|h| world.inspect(h)) else {
                continue;
            };
            if !hv.is(class::EQ_OBJ) || (hv.class != class::SOLAR && hv.class != class::SHIP) {
                continue;
            }
            if hv.id == target.id && target.class & 1 == 1 {
                continue;
            }
            let hx = e(hit.point.x) - e(barrel.x);
            let hy = e(hit.point.y) - e(barrel.y);
            let hz = e(hit.point.z) - e(barrel.z);
            let tx = e(tp.x) - e(barrel.x);
            let ty = e(tp.y) - e(barrel.y);
            let tz = r(e(tp.z) - e(barrel.z));
            let hit_sq = (hz * hz + hy * hy) + hx * hx;
            let target_sq = (e(tz) * e(tz) + ty * ty) + tx * tx;
            if target_sq > hit_sq {
                slot.blocked = true;
            }
            return slot.blocked;
        }
        slot.blocked
    }

    /// `CanFireLauncherAtTarget(id, target, range)` ([`can_fire`]).
    pub fn can_fire_launcher_at_target<
        W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized,
    >(
        &mut self,
        world: &W,
        ship: W::Handle,
        id: u16,
        target: Option<&ObjectView>,
        range: f32,
    ) -> i32 {
        let Some(target) = target else {
            return can_fire::NOT_READY;
        };
        if world.inspect(ship).is_none() {
            return can_fire::NOT_READY;
        }
        let info = world.equipment(ship, id);
        let (flags, equip_flags) = info.map_or((0, 0), |i| (i.flags, i.equip_flags));
        let mut result = can_fire::NOT_READY;
        if info.is_some() && flags & 1 != 0 && flags & 6 == 0 && equip_flags & 0xa != 0 {
            let Some((barrel, _)) = info.and_then(|i| i.barrel) else {
                return can_fire::INACTIVE;
            };
            let p = target.position;
            let dx = e(p.x) - e(barrel.x);
            let dy = e(p.y) - e(barrel.y);
            let dz = e(p.z) - e(barrel.z);
            let dist = inline_fast_length((dz * dz + dy * dy) + dx * dx);
            if !(dist < e(range)) {
                return can_fire::OUT_OF_RANGE;
            }
            result = can_fire::OK;
        } else {
            if flags & 1 == 0 {
                result = can_fire::INACTIVE;
            }
            if equip_flags & 4 != 0 {
                result = can_fire::FLAG_4;
            }
            if equip_flags & 8 != 0 {
                return can_fire::FLAG_8;
            }
            if result != can_fire::OK {
                return result;
            }
        }
        if self.is_line_of_fire_blocked(world, ship, target, id) {
            result = can_fire::BLOCKED;
        }
        result
    }

    /// `UpdateGunFireState`: aim and fire the ready plain guns (all of
    /// them, or the first, by fire style) at the aim point, which is
    /// re-chosen in the target's frame every `gun_target_point_switch_time`.
    pub fn update_gun_fire_state<W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized>(
        &mut self,
        env: &mut FireEnv<'_, W>,
    ) -> i32 {
        if env.selection_priority & 1 == 0 {
            return can_fire::NO_GUNS;
        }
        if env.world.inspect(env.ship).is_none() {
            return 0;
        }
        let Some((_, target)) = env.view_of(self.target) else {
            return 0;
        };
        let (hold, spread) = if self.hold_mask & 0x3000_0000 != 0 {
            (true, 0.0)
        } else {
            (false, self.gun_fire_accuracy_cone_angle)
        };
        let force = env.timers.active(slot::FIRE_GUNS);
        let mut result = can_fire::NOT_READY;
        let xform = target.transform();
        if self.aim_refresh_timer <= 0.0 {
            let lead = Self::lead_fire_position(env, &target, false, spread, None);
            self.aim_offset = inverse_transform(&xform, lead);
            self.aim_refresh_timer = env.personality.gun.target_point_switch_time;
        }
        let mut fire_ids = Vec::new();
        let guns = self.guns.clone();
        for slot in &guns {
            if !slot.ready() && !hold && !force {
                continue;
            }
            if slot.kind != kind::PLAIN || slot.auto_turret {
                continue;
            }
            let current = env.view_of(self.target).map(|(_, v)| v);
            result = self.can_fire_launcher_at_target(
                &*env.world,
                env.ship,
                slot.id,
                current.as_ref(),
                self.gun_range,
            );
            if result != can_fire::OK && !force {
                continue;
            }
            env.aim_gun(slot.id, self.target);
            if env
                .world
                .is_pointing_at(env.ship, slot.id, GUN_POINTING_ANGLE)
                || force
            {
                fire_ids.push(slot.id);
            }
            if !self.fire_all_guns {
                break;
            }
        }
        if fire_ids.is_empty() {
            return result;
        }
        let Some((_, target)) = env.view_of(self.target) else {
            return result;
        };
        let own_is_ship = env
            .world
            .inspect(env.ship)
            .is_some_and(|v| v.is(class::SHIP));
        let lead = own_is_ship
            .then(|| env.world.lead_fire_position(env.ship, None))
            .flatten()
            .unwrap_or(target.position);
        let xform = Transform::new(target.orientation, lead);
        let fire_pos = transform_xform(&xform, self.aim_offset);
        env.fire(fire_ids, Some(fire_pos));
        result
    }

    /// `UpdateMissileFireState`: fire each ready launcher whose munition
    /// the selected preference allows (seekers, dumb missiles, type 0xa)
    /// or, for a cruise disruptor, at a target cruising long enough within
    /// its distance band.
    pub fn update_missile_fire_state<
        W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized,
    >(
        &mut self,
        env: &mut FireEnv<'_, W>,
    ) -> i32 {
        let mut result = 0;
        let Some((_, target)) = env.view_of(self.target) else {
            return 0;
        };
        let Some(own) = env.world.inspect(env.ship) else {
            return 0;
        };
        let flags = env.selection_priority;
        if self.hold_mask & 0x3000_0000 != 0 {
            return 2;
        }
        let force = env.timers.active(slot::FIRE_MISSILES);
        let m = &env.personality.missile;
        for index in 0..self.guns.len() {
            let slot: GunSlot = self.guns[index];
            if !(slot.phase >= 0.0) || !slot.fire_tick {
                continue;
            }
            let mut wide = false;
            match slot.kind {
                kind::SEEKER => {
                    if slot.seeker != 0 {
                        if flags & 2 == 0 {
                            continue;
                        }
                    } else {
                        wide = true;
                        if flags & 4 == 0 {
                            continue;
                        }
                    }
                }
                kind::HP_TYPE_10 => {
                    wide = true;
                    if flags & 8 == 0 {
                        continue;
                    }
                }
                kind::CRUISE_DISRUPTOR => {
                    if !(self.cruise_held_time > m.anti_cruise_pre_fire_delay) {
                        continue;
                    }
                    let p = target.position;
                    let dx = e(p.x) - e(own.center.x);
                    let dy = e(p.y) - e(own.center.y);
                    let dz = e(p.z) - e(own.center.z);
                    let dist = inline_fast_length((dz * dz + dy * dy) + dx * dx);
                    if !(dist > e(m.anti_cruise_min_distance)) {
                        continue;
                    }
                    if !(dist < e(m.anti_cruise_max_distance)) {
                        continue;
                    }
                }
                _ => continue,
            }
            result = self.can_fire_launcher_at_target(
                &*env.world,
                env.ship,
                slot.id,
                Some(&target),
                self.missile_launch_range,
            );
            if result != can_fire::OK {
                continue;
            }
            env.aim_gun(slot.id, self.target);
            let angle = if wide {
                self.wide_cone_angle
            } else {
                self.missile_launch_cone_angle
            };
            if !env.world.is_pointing_at(env.ship, slot.id, angle) && !force {
                continue;
            }
            env.fire(vec![slot.id], None);
            if slot.kind != kind::PLAIN {
                self.guns[index].phase = -slot.fire_interval;
            }
        }
        result
    }

    /// `UpdateMineDropState`: drop mines from the ready droppers while the
    /// ship moves faster than 20 m/s and a hostile follows it inside the
    /// mine cone (or the drop-mines timer is latched). 1 when a dropper
    /// was ready.
    pub fn update_mine_drop_state<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        env: &mut FireEnv<'_, W>,
        list: &mut TargetList,
    ) -> u32 {
        let mut result = 0;
        if env.view_of(self.target).is_none() {
            return 0;
        }
        let Some(own) = env.world.inspect(env.ship) else {
            return 0;
        };
        if self.hold_mask & 0x3000_0000 != 0 {
            return 2;
        }
        let aggressive = env.timers.active(slot::DROP_MINES);
        let v = own.velocity;
        let speed = ((e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)).sqrt();
        if !(speed > 20.0) {
            return result;
        }
        if !aggressive
            && !has_target_in_forward_arc(
                &*env.world,
                env.ship,
                list,
                self.mine_launch_cone_angle,
                env.personality.mine.launch_range,
            )
        {
            return result;
        }
        for index in 0..self.mine_droppers.len() {
            let slot = self.mine_droppers[index];
            if !aggressive && (!(slot.phase >= 0.0) || !slot.fire_tick) {
                continue;
            }
            let equip_flags = env
                .world
                .equipment(env.ship, slot.id)
                .map_or(0, |i| i.equip_flags);
            if equip_flags & 0xa != 0xa {
                continue;
            }
            result = 1;
            env.fire(vec![slot.id], None);
            if slot.kind != kind::PLAIN {
                self.mine_droppers[index].phase = -slot.fire_interval;
            }
        }
        result
    }

    /// `DropAllMines`.
    pub fn drop_all_mines<W: AiWorld + ?Sized>(&self, world: &mut W, ship: W::Handle) {
        for slot in &self.mine_droppers {
            world.command(
                ship,
                AiCommand::FireGuns {
                    equipment: vec![slot.id],
                    target: None,
                },
            );
        }
    }

    /// `FireAutoTurrets`: while attacking, fire each ready (or, with the
    /// fire-guns timer, every) auto turret that has a target, may fire at
    /// it and points at it, at its lead position.
    pub fn fire_auto_turrets<W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized>(
        &mut self,
        env: &mut FireEnv<'_, W>,
    ) -> bool {
        if !self.enabled || env.attack_state != 0 {
            return false;
        }
        let Some(own) = env.world.inspect(env.ship) else {
            return false;
        };
        let spread = if own.is_player {
            0.0
        } else {
            self.gun_fire_accuracy_cone_angle
        };
        let force = env.timers.active(slot::FIRE_GUNS);
        for index in 0..self.guns.len() {
            let slot = self.guns[index];
            if !slot.ready() && !force {
                continue;
            }
            if !slot.auto_turret {
                continue;
            }
            let Some(target_id) = env
                .world
                .equipment(env.ship, slot.id)
                .and_then(|i| i.target_id)
            else {
                continue;
            };
            if target_id & 0x3fff_ffff == 0 {
                continue;
            }
            let Some((_, target)) = env.view_of(Some(target_id)) else {
                continue;
            };
            let range = self.gun_range;
            if self.can_fire_launcher_at_target(
                &*env.world,
                env.ship,
                slot.id,
                Some(&target),
                range,
            ) != can_fire::OK
            {
                continue;
            }
            if !env
                .world
                .is_pointing_at(env.ship, slot.id, GUN_POINTING_ANGLE)
            {
                continue;
            }
            let pos = Self::lead_fire_position(env, &target, true, spread, None);
            env.fire(vec![slot.id], Some(pos));
            if slot.kind != kind::PLAIN {
                self.guns[index].phase = -slot.fire_interval;
            }
        }
        true
    }

    /// `RunFireControlTick(target)`: retarget, then the guns, the
    /// launchers and the mine droppers.
    pub fn run_fire_control_tick<W: AiWorld + AiRand + super::world::SpatialQueries + ?Sized>(
        &mut self,
        env: &mut FireEnv<'_, W>,
        list: &mut TargetList,
        target: Option<u32>,
    ) -> bool {
        if !self.enabled {
            return false;
        }
        if !self.retarget(env.world, env.ship, target) {
            return false;
        }
        self.last_gun_fire_result = self.update_gun_fire_state(env);
        self.update_missile_fire_state(env);
        self.update_mine_drop_state(env, list);
        true
    }
}

/// `SystemManagerTimerTable::HasTargetInForwardArc(angle, range)`: a
/// hostile ship-class object behind the ship (`dot(forward, own - other)
/// > 0`), within range, faster than 10 m/s, heading the same way (cosine
/// above 0.95) and within `max_angle` of the ship's tail.
pub fn has_target_in_forward_arc<W: AiWorld + ?Sized>(
    world: &W,
    ship: W::Handle,
    list: &mut TargetList,
    max_angle: f32,
    max_range: f32,
) -> bool {
    let Some(own) = world.inspect(ship) else {
        return false;
    };
    for (h, v) in list.views(world, ship) {
        if v.type_flags & 0x7f_0000 == 0 {
            continue;
        }
        let d = sub(own.center, v.center);
        let own_fwd = neg(column2(&own.orientation));
        let dot = dot_zyx(own_fwd, d);
        let dot_f = r(dot);
        if !(dot > 0.0) {
            continue;
        }
        let dist_sq = (e(d.z) * e(d.z) + e(d.y) * e(d.y)) + e(d.x) * e(d.x);
        if !(e(max_range) * e(max_range) > dist_sq) {
            continue;
        }
        let vel = v.velocity;
        let speed_sq = (e(vel.z) * e(vel.z) + e(vel.y) * e(vel.y)) + e(vel.x) * e(vel.x);
        if !(speed_sq > 100.0) {
            continue;
        }
        let tgt_fwd = neg(column2(&v.orientation));
        let heading = (e(own_fwd.z) * e(tgt_fwd.z) + e(own_fwd.y) * e(tgt_fwd.y))
            + e(tgt_fwd.x) * e(own_fwd.x);
        if !(heading > e(0.95)) {
            continue;
        }
        let (attitude, _) = world.attitude_symmetrical(ship, h).unwrap_or((0.0, 0.0));
        if !(attitude <= -0.6) {
            continue;
        }
        let ratio = e(dot_f) / dist_sq.sqrt();
        let cosine = if 1.0 < ratio { 1.0 } else { r(ratio) };
        if e(cosine).acos() <= e(max_angle) {
            return true;
        }
    }
    false
}

/// An item that is on (`EquipStatus` bit 0).
#[must_use]
pub fn is_active(flags: u8) -> bool {
    flags & EQUIP_ACTIVE != 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::world::RecordedRand;

    #[test]
    fn lead_offset_without_spread_is_the_lead() {
        let lead = Vector::new(0.0, 0.0, 100.0);
        // A slow player: one draw, no random vector.
        let mut rand = RecordedRand::new(vec![30_000]);
        let p = lead_fire_offset(
            lead,
            Vector::ZERO,
            true,
            Vector::ZERO,
            1.0,
            6.0,
            0.05,
            &mut rand,
        );
        assert_eq!(p, lead);
        assert!(rand.exhausted_exactly());
        // A small spread: four draws, the lead unchanged.
        let mut rand = RecordedRand::new(vec![100, 1, 2, 3]);
        let p = lead_fire_offset(
            lead,
            Vector::ZERO,
            false,
            Vector::ZERO,
            1.0,
            6.0,
            0.05,
            &mut rand,
        );
        assert_eq!(p, lead);
        assert!(rand.exhausted_exactly());
    }

    #[test]
    fn lead_offset_spreads_perpendicular() {
        let lead = Vector::new(0.0, 0.0, 100.0);
        let mut rand = RecordedRand::new(vec![32_767, 32_767, 0x3fff, 0x3fff]);
        let p = lead_fire_offset(
            lead,
            Vector::ZERO,
            false,
            Vector::ZERO,
            1.0,
            1.0,
            0.1,
            &mut rand,
        );
        assert!(rand.exhausted_exactly());
        // Off along x (dir × (1, 0, 0) is (0, 100, 0) normalised... the
        // random vector is (1, 0, 0) here), by about 100 * tan(0.1).
        let off = ((e(p.x) * e(p.x)) + e(p.y) * e(p.y)).sqrt();
        assert!((off - 100.0 * 0.1_f64.tan()).abs() < 1.0e-3, "{p:?}");
        assert_eq!(p.z, 100.0);
    }
}
