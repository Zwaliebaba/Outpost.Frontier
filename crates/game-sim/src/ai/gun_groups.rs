//! The system manager's gun groups (`SystemManagerGunGroupManager`,
//! `Common/common_sysmgr_gungroups.cpp`): one timing slot per gun,
//! launcher and mine dropper, the auto-turret target scan, and the
//! countermeasure droppers' duty cycle (`FCmDropperWatcher`,
//! `Common/common_aifirecontrol.cpp`). The firing decisions over these
//! slots are in [`super::fire_control`].
//!
//! Each slot runs a burst cycle: its phase climbs from `-no_burst_time`
//! (or a random start) past zero to `burst_time`, and while it is at or
//! above zero the slot may fire on each tick its phase crosses a multiple
//! of the fire interval. The intervals are the personality's, jittered by
//! `rand()` when the slots are built; the order of those draws is the
//! reference's, so a seeded run draws the same values.

use neuron_core::math::Vector;

use super::fp::{e, r};
use super::personality::Personality;
use super::target_manager::matches_attack_order;
use super::world::{AiCommand, AiRand, AiWorld, EQUIP_ACTIVE, class};

/// `EquipTypeGun`.
pub const EQUIP_GUN: u32 = 0x80;
/// `EquipTypeMineDropper`.
pub const EQUIP_MINE_DROPPER: u32 = 0x20;
/// `EquipTypeCmDropper`.
pub const EQUIP_CM_DROPPER: u32 = 0x40;
/// `EquipTypeShield`.
pub const EQUIP_SHIELD: u32 = 0x100;

/// The most slots of each array.
pub const MAX_SLOTS: usize = 0x40;

/// `FGunGroupSlot::MunitionKind`.
pub mod kind {
    /// A munition without a seeker.
    pub const PLAIN: i32 = 0;
    /// A munition with a seeker.
    pub const SEEKER: i32 = 1;
    /// A munition of hardpoint type 0xa.
    pub const HP_TYPE_10: i32 = 2;
    /// A mine dropper.
    pub const MINE_DROPPER: i32 = 3;
    /// A cruise disruptor.
    pub const CRUISE_DISRUPTOR: i32 = 4;
    /// Not classified yet.
    pub const UNCLASSIFIED: i32 = 5;
}

/// `FGunGroupSlot` (0x20 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GunSlot {
    /// +0x00 the item's id.
    pub id: u16,
    /// +0x02 an auto turret.
    pub auto_turret: bool,
    /// +0x03 the munition's seeker byte.
    pub seeker: u8,
    /// +0x04 the phase crossed a fire-interval boundary this tick.
    pub fire_tick: bool,
    /// +0x08 the munition kind ([`kind`]).
    pub kind: i32,
    /// +0x0C the phase runs up to this ...
    pub burst_time: f32,
    /// +0x10 ... then restarts at minus this.
    pub no_burst_time: f32,
    /// +0x14 the fire interval.
    pub fire_interval: f32,
    /// +0x18 the phase, at or above zero inside a burst.
    pub phase: f32,
}

impl Default for GunSlot {
    /// `InitGunGroupArray`'s values.
    fn default() -> Self {
        Self {
            id: 0,
            auto_turret: false,
            seeker: 0,
            fire_tick: false,
            kind: 0,
            burst_time: 3.0,
            no_burst_time: 1.5,
            fire_interval: 0.5,
            phase: 0.0,
        }
    }
}

impl GunSlot {
    /// Inside a burst and on a fire tick.
    #[must_use]
    pub fn ready(&self) -> bool {
        self.phase >= 0.0 && self.fire_tick
    }

    /// `StepGunGroupPhase(slot, dt)`.
    pub fn step(&mut self, dt: f32) {
        let phase = self.phase;
        if self.kind != kind::PLAIN {
            self.fire_tick = true;
            self.phase = r(e(dt) + e(phase));
            return;
        }
        let period = self.fire_interval;
        if !(period > 0.0) {
            self.fire_tick = true;
        } else {
            let a = (e(dt) + e(phase)) % e(period);
            let b = e(phase) % e(period);
            self.fire_tick = b > a;
        }
        let next = e(dt) + e(phase);
        self.phase = r(next);
        if next > e(self.burst_time) {
            self.phase = -self.no_burst_time;
        }
    }
}

/// `FLauncherFireCacheSlot`: one cached line-of-fire answer.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LineOfFireSlot {
    /// +0x00 claimed.
    pub in_use: bool,
    /// +0x04 the target's id.
    pub target_id: u32,
    /// +0x08 the launcher's id.
    pub hardpoint: u16,
    /// +0x0C time since claimed or recomputed; evicted past 10 s.
    pub age: f32,
    /// +0x10 counts to 1 s while fresh.
    pub refresh_timer: f32,
    /// +0x14 the next query recomputes.
    pub needs_refresh: bool,
    /// +0x15 the cached answer.
    pub blocked: bool,
}

impl Default for LineOfFireSlot {
    fn default() -> Self {
        Self {
            in_use: false,
            target_id: 0,
            hardpoint: 0,
            age: 0.0,
            refresh_timer: 0.0,
            needs_refresh: true,
            blocked: false,
        }
    }
}

/// `FCmDropperWatcher`: the countermeasure droppers, on for
/// `countermeasure_active_time` of every `countermeasure_unactive_time`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CmDroppers {
    /// +0x04 the cycle timer.
    pub timer: f32,
    /// +0x08 the state last applied.
    pub active: bool,
    /// +0x10 the droppers' ids.
    pub ids: Vec<u16>,
    /// The ship is the player (`Bind` and `Update` do nothing for it).
    pub player: bool,
}

impl CmDroppers {
    /// `FCmDropperWatcher::SetActive`: switch every dropper whose state
    /// differs.
    pub fn set_active<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle, on: bool) {
        if self.active == on {
            return;
        }
        for &id in &self.ids {
            let Some(info) = world.equipment(ship, id) else {
                continue;
            };
            if (info.flags & EQUIP_ACTIVE != 0) == on {
                continue;
            }
            world.command(ship, AiCommand::ActivateEquip { equipment: id, on });
        }
        self.active = on;
    }

    /// `FCmDropperWatcher::Bind`: find the droppers (at most 0x40, up to
    /// the first id 0) and switch them on.
    pub fn bind<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) {
        let Some(view) = world.inspect(ship) else {
            return;
        };
        self.player = view.is_player;
        if view.is_player {
            return;
        }
        self.active = false;
        self.timer = 0.0;
        self.ids = world
            .find_equipment(ship, EQUIP_CM_DROPPER)
            .map(|ids| {
                ids.into_iter()
                    .take(MAX_SLOTS)
                    .take_while(|&id| id != 0)
                    .collect()
            })
            .unwrap_or_default();
        self.set_active(world, ship, true);
    }

    /// `FCmDropperWatcher::Update(dt)`.
    pub fn update<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        dt: f32,
    ) {
        if self.player {
            return;
        }
        let cm = &p.countermeasure;
        let sum = e(dt) + e(self.timer);
        self.timer = r(sum);
        if sum >= e(cm.unactive_time) {
            self.timer = 0.0;
        }
        if self.timer < cm.active_time {
            self.set_active(world, ship, true);
            return;
        }
        if !(self.timer < cm.unactive_time) {
            return;
        }
        if world.inspect(ship).is_some_and(|v| !v.is_player) {
            self.set_active(world, ship, false);
        }
    }
}

/// `SystemManagerGunGroupManager` (`SystemManager+0x12C`).
#[derive(Clone, Debug, PartialEq)]
pub struct GunGroups {
    /// +0x00 `UpdateGunFireState`'s last result.
    pub last_gun_fire_result: i32,
    /// +0x08 the target the guns are aimed at (`TargetWatcher`).
    pub target: Option<u32>,
    /// +0x10 bits 0x30000000 hold all fire (no writer in Common).
    pub hold_mask: u32,
    /// +0x14 the aim point in the target's frame.
    pub aim_offset: Vector,
    /// +0x20 the aim point is recomputed once this is at or below 0.
    pub aim_refresh_timer: f32,
    /// +0x24 how long the target has been in cruise.
    pub cruise_held_time: f32,
    /// +0x28 the auto-turret scan timer.
    pub scan_timer: f32,
    /// +0x2C its period (1 s).
    pub scan_period: f32,
    /// +0x30 the guns and launchers.
    pub guns: Vec<GunSlot>,
    /// +0x838 the mine droppers.
    pub mine_droppers: Vec<GunSlot>,
    /// +0x103C `gun_fire_interval_time`.
    pub gun_fire_interval_time: f32,
    /// +0x1040 `gun_fire_interval_variance_percent`.
    pub gun_fire_interval_variance: f32,
    /// +0x1044 `auto_turret_interval_time`.
    pub auto_turret_interval_time: f32,
    /// +0x1048 `gun_fire_burst_interval_time`.
    pub gun_fire_burst_interval_time: f32,
    /// +0x104C `gun_fire_burst_interval_variance_percent`.
    pub gun_fire_burst_interval_variance: f32,
    /// +0x1050 `gun_fire_no_burst_interval_time`.
    pub gun_fire_no_burst_interval_time: f32,
    /// +0x1054 `auto_turret_burst_interval_time`.
    pub auto_turret_burst_interval_time: f32,
    /// +0x1058 `auto_turret_no_burst_interval_time`.
    pub auto_turret_no_burst_interval_time: f32,
    /// +0x105C `auto_turret_burst_interval_variance_percent`.
    pub auto_turret_burst_interval_variance: f32,
    /// +0x1060 `missile_launch_interval_time`.
    pub missile_launch_interval_time: f32,
    /// +0x1064 `missile_launch_interval_variance_percent`.
    pub missile_launch_interval_variance: f32,
    /// +0x1068 `missile_launch_cone_angle`.
    pub missile_launch_cone_angle: f32,
    /// +0x106C `missile_launch_range`.
    pub missile_launch_range: f32,
    /// +0x1070 `missile_launch_allow_out_of_range` as a float.
    pub missile_launch_allow_out_of_range: f32,
    /// +0x1074 `mine_launch_interval`.
    pub mine_launch_interval: f32,
    /// +0x1078 `mine_launch_cone_angle`.
    pub mine_launch_cone_angle: f32,
    /// +0x1080 `gun_fire_accuracy_cone_angle`.
    pub gun_fire_accuracy_cone_angle: f32,
    /// +0x1084 the longest plain-gun range times the jittered
    /// `gun_range_threshold`.
    pub gun_range: f32,
    /// +0x1088 fire every ready gun (`fire_style` 0) or the first one.
    pub fire_all_guns: bool,
    /// +0x1090 the wide aim angle of `UpdateMissileFireState`.
    pub wide_cone_angle: f32,
    /// +0x1094 enabled.
    pub enabled: bool,
    /// +0x1098 the line-of-fire cache.
    pub line_of_fire: [LineOfFireSlot; 16],
    /// +0x1218 the countermeasure droppers.
    pub cm: CmDroppers,
}

impl Default for GunGroups {
    /// `ConstructSystemManagerGunGroupManager`.
    fn default() -> Self {
        Self {
            last_gun_fire_result: 0,
            target: None,
            hold_mask: 0,
            aim_offset: Vector::ZERO,
            aim_refresh_timer: 0.0,
            cruise_held_time: 0.0,
            scan_timer: 0.0,
            scan_period: 1.0,
            guns: Vec::new(),
            mine_droppers: Vec::new(),
            gun_fire_interval_time: 0.5,
            gun_fire_interval_variance: 0.2,
            auto_turret_interval_time: 0.5,
            gun_fire_burst_interval_time: 4.0,
            gun_fire_burst_interval_variance: 0.3,
            gun_fire_no_burst_interval_time: 2.0,
            auto_turret_burst_interval_time: 4.0,
            auto_turret_no_burst_interval_time: 2.0,
            auto_turret_burst_interval_variance: 0.3,
            missile_launch_interval_time: 3.0,
            missile_launch_interval_variance: 0.5,
            missile_launch_cone_angle: core::f32::consts::PI,
            missile_launch_range: 800.0,
            missile_launch_allow_out_of_range: 0.0,
            mine_launch_interval: 5.0,
            mine_launch_cone_angle: core::f32::consts::FRAC_PI_8,
            gun_fire_accuracy_cone_angle: 0.05,
            gun_range: 1100.0,
            fire_all_guns: false,
            wide_cone_angle: core::f32::consts::PI / 32.0,
            enabled: true,
            line_of_fire: [LineOfFireSlot::default(); 16],
            cm: CmDroppers::default(),
        }
    }
}

/// `rand() * (1/32768f)`, on the stack.
fn rand_unit(rand: &mut impl AiRand) -> f64 {
    f64::from(rand.rand()) * e(1.0 / 32768.0)
}

/// `JitterAround(half, scale)`: `((half - -half) * unit + -half) * scale +
/// scale`, one draw.
fn jitter(rand: &mut impl AiRand, half: f32, scale: f32) -> f32 {
    let neg = -half;
    let unit = rand_unit(rand);
    r(((e(half) - e(neg)) * unit + e(neg)) * e(scale) + e(scale))
}

impl GunGroups {
    /// `RandomizeSlotTiming`: the slot's burst, pause and interval for its
    /// kind, jittered, then a random starting phase in `[-interval, 0]`.
    /// Draws 4 values for a plain gun or turret, 2 for a seeker, a type
    /// 0xa munition or a mine dropper, 1 otherwise.
    pub fn randomize_slot_timing(
        &self,
        slot: &mut GunSlot,
        p: &Personality,
        rand: &mut impl AiRand,
    ) {
        let interval = match slot.kind {
            kind::PLAIN if !slot.auto_turret => {
                slot.burst_time = jitter(
                    rand,
                    self.gun_fire_burst_interval_variance,
                    self.gun_fire_burst_interval_time,
                );
                slot.no_burst_time = jitter(
                    rand,
                    self.gun_fire_burst_interval_variance,
                    self.gun_fire_no_burst_interval_time,
                );
                Some(jitter(
                    rand,
                    self.gun_fire_interval_variance,
                    self.gun_fire_interval_time,
                ))
            }
            kind::PLAIN => {
                slot.burst_time = jitter(
                    rand,
                    self.auto_turret_burst_interval_variance,
                    self.auto_turret_burst_interval_time,
                );
                slot.no_burst_time = jitter(
                    rand,
                    self.auto_turret_burst_interval_variance,
                    self.auto_turret_no_burst_interval_time,
                );
                let unit = rand_unit(rand);
                Some(r((unit * 0.5 + 0.75) * e(self.auto_turret_interval_time)))
            }
            kind::SEEKER => Some(jitter(
                rand,
                self.missile_launch_interval_variance,
                self.missile_launch_interval_time,
            )),
            kind::HP_TYPE_10 | kind::MINE_DROPPER => {
                slot.burst_time = 0.0;
                slot.no_burst_time = 0.0;
                let unit = rand_unit(rand);
                Some(r(
                    (unit * e(0.400_000_04) + e(0.8)) * e(self.missile_launch_interval_time)
                ))
            }
            kind::CRUISE_DISRUPTOR => {
                slot.fire_interval = p.missile.anti_cruise_interval_time;
                None
            }
            _ => None,
        };
        if let Some(interval) = interval {
            slot.fire_interval = interval;
        }
        let neg = -slot.fire_interval;
        let unit = rand_unit(rand);
        slot.phase = r(-e(neg) * unit + e(neg));
    }

    /// `PopulateSlots`: a slot per id up to the first id 0, classified by
    /// the item's values; an inactive, working item of an AI ship is
    /// switched on.
    fn populate<W: AiWorld + ?Sized>(
        world: &mut W,
        ship: W::Handle,
        ids: &[u16],
        mine_droppers: bool,
    ) -> Vec<GunSlot> {
        let ids: Vec<u16> = ids
            .iter()
            .copied()
            .take(MAX_SLOTS)
            .take_while(|&id| id != 0)
            .collect();
        let is_player = world.inspect(ship).is_some_and(|v| v.is_player);
        let mut slots = Vec::with_capacity(ids.len());
        for id in ids {
            let mut slot = GunSlot {
                id,
                auto_turret: false,
                kind: kind::UNCLASSIFIED,
                ..GunSlot::default()
            };
            let info = world.equipment(ship, id);
            if mine_droppers {
                slot.kind = kind::MINE_DROPPER;
            } else if let Some(k) = info.and_then(|i| i.munition_kind) {
                slot.kind = match k {
                    0 => kind::PLAIN,
                    1 => kind::SEEKER,
                    2 => kind::HP_TYPE_10,
                    3 => kind::CRUISE_DISRUPTOR,
                    _ => slot.kind,
                };
            }
            if info.and_then(|i| i.auto_turret) == Some(true) {
                slot.auto_turret = true;
            }
            if let Some(seeker) = info.and_then(|i| i.seeker) {
                slot.seeker = seeker;
            }
            if let Some(i) = info
                && !is_player
                && i.flags & EQUIP_ACTIVE == 0
            {
                world.command(
                    ship,
                    AiCommand::ActivateEquip {
                        equipment: id,
                        on: true,
                    },
                );
            }
            slots.push(slot);
        }
        slots
    }

    /// `ResolveHardpoints`: the gun and mine-dropper slots.
    pub fn resolve_hardpoints<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) {
        self.guns.clear();
        if let Some(ids) = world.find_equipment(ship, EQUIP_GUN) {
            self.guns = Self::populate(world, ship, &ids, false);
        }
        self.mine_droppers.clear();
        if let Some(ids) = world.find_equipment(ship, EQUIP_MINE_DROPPER) {
            self.mine_droppers = Self::populate(world, ship, &ids, true);
        }
    }

    /// The gun-group part of `InitSystemManager`: resolve the slots,
    /// randomise each gun's timing, then the scan timer's phase (in that
    /// order of `rand()` draws).
    pub fn init<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
    ) {
        self.resolve_hardpoints(world, ship);
        let mut guns = core::mem::take(&mut self.guns);
        for slot in &mut guns {
            self.randomize_slot_timing(slot, p, &mut &mut *world);
        }
        self.guns = guns;
        self.scan_period = 1.0;
        self.scan_timer = r(f64::from(world.rand()) * e(1.0 / 32768.0) * e(self.scan_period));
    }

    /// `SetShieldActive`: switch the first shield on or off.
    pub fn set_shield_active<W: AiWorld + ?Sized>(world: &mut W, ship: W::Handle, on: bool) {
        if let Some(&id) = world
            .find_equipment(ship, EQUIP_SHIELD)
            .as_ref()
            .and_then(|ids| ids.first())
        {
            world.command(ship, AiCommand::ActivateEquip { equipment: id, on });
        }
    }

    /// `ClearGunTargets`.
    pub fn clear_gun_targets<W: AiWorld + ?Sized>(&self, world: &mut W, ship: W::Handle) {
        for slot in &self.guns {
            world.command(
                ship,
                AiCommand::SetGunTarget {
                    equipment: slot.id,
                    target: None,
                    subtarget: 0,
                },
            );
        }
    }

    /// `Retarget(target)`: aim the ship and (for an AI ship) every gun at
    /// a new target; false for none.
    pub fn retarget<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        target: Option<u32>,
    ) -> bool {
        let Some(target) = target else {
            return false;
        };
        if self.target == Some(target) {
            return true;
        }
        let h = world.handle_of(target);
        self.target = h.and(Some(target));
        world.command(
            ship,
            AiCommand::SetTarget {
                target: h,
                subtarget: 0,
            },
        );
        if world.inspect(ship).is_some_and(|v| v.is_player) {
            return true;
        }
        for slot in &self.guns {
            world.command(
                ship,
                AiCommand::SetGunTarget {
                    equipment: slot.id,
                    target: h,
                    subtarget: 0,
                },
            );
        }
        true
    }

    /// `UpdateTimers(dt)`: the aim refresh countdown, the line-of-fire
    /// cache's ageing, the target's cruise time and each slot's phase.
    pub fn update_timers<W: AiWorld + ?Sized>(&mut self, world: &W, dt: f32) {
        let cruising = self
            .target
            .and_then(|id| world.handle_of(id))
            .and_then(|h| world.inspect(h))
            .map(|v| v.cruise.is_some_and(|c| c.0));
        self.step_timers(cruising, dt);
    }

    /// [`Self::update_timers`] with the target's cruise state given
    /// (`None` when no target is watched).
    pub fn step_timers(&mut self, target_cruising: Option<bool>, dt: f32) {
        self.aim_refresh_timer = r(e(self.aim_refresh_timer) - e(dt));
        for slot in &mut self.line_of_fire {
            if !slot.in_use {
                continue;
            }
            let age = r(e(dt) + e(slot.age));
            slot.age = age;
            if !slot.needs_refresh {
                let refresh = e(dt) + e(slot.refresh_timer);
                slot.refresh_timer = r(refresh);
                if refresh >= 1.0 {
                    slot.refresh_timer = 0.0;
                    slot.needs_refresh = true;
                }
            }
            if age > 10.0 {
                *slot = LineOfFireSlot::default();
            }
        }
        self.cruise_held_time = match target_cruising {
            Some(true) => r(e(dt) + e(self.cruise_held_time)),
            _ => 0.0,
        };
        for slot in &mut self.guns {
            slot.step(dt);
        }
        for slot in &mut self.mine_droppers {
            slot.step(dt);
        }
    }

    /// `RefreshFromPersonality` with the personality to copy (the formation
    /// leader's when it makes the ship tougher): the intervals, the gun
    /// range from the longest plain-gun range times a jittered threshold
    /// (one draw), and every slot's timing again.
    pub fn refresh_from_personality<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
    ) {
        let g = &p.gun;
        self.gun_fire_interval_time = g.fire_interval_time;
        self.gun_fire_interval_variance = g.fire_interval_variance_percent;
        self.auto_turret_interval_time = g.auto_turret_interval_time;
        self.gun_fire_burst_interval_time = g.fire_burst_interval_time;
        self.gun_fire_burst_interval_variance = g.fire_burst_interval_variance_percent;
        self.auto_turret_burst_interval_time = g.auto_turret_burst_interval_time;
        self.auto_turret_burst_interval_variance = g.auto_turret_burst_interval_variance_percent;
        self.gun_fire_no_burst_interval_time = g.fire_no_burst_interval_time;
        self.auto_turret_no_burst_interval_time = g.auto_turret_no_burst_interval_time;
        self.gun_fire_accuracy_cone_angle = g.fire_accuracy_cone_angle;
        let spread = g.range_threshold_variance_percent;
        let neg = -spread;
        let unit = rand_unit(&mut &mut *world);
        let value = r(((e(spread) - e(neg)) * unit + e(neg) + 1.0) * e(g.range_threshold));
        if let Some((_, max)) = self.gun_range_stats(world, ship) {
            self.gun_range = r(e(max) * e(value));
        }
        self.missile_launch_range = p.missile.launch_range;
        self.missile_launch_allow_out_of_range = f32::from(p.missile.launch_allow_out_of_range);
        match g.fire_style {
            0 => self.fire_all_guns = true,
            1 => self.fire_all_guns = false,
            _ => {}
        }
        self.missile_launch_interval_time = p.missile.launch_interval_time;
        self.missile_launch_interval_variance = p.missile.launch_interval_variance_percent;
        self.missile_launch_cone_angle = p.missile.launch_cone_angle;
        self.mine_launch_interval = p.mine.launch_interval;
        self.mine_launch_cone_angle = p.mine.launch_cone_angle;
        let mut guns = core::mem::take(&mut self.guns);
        for slot in &mut guns {
            self.randomize_slot_timing(slot, p, &mut &mut *world);
        }
        self.guns = guns;
        let mut mines = core::mem::take(&mut self.mine_droppers);
        for slot in &mut mines {
            self.randomize_slot_timing(slot, p, &mut &mut *world);
        }
        self.mine_droppers = mines;
    }

    /// `ComputeGunRangeStats`: the average and the longest range of the
    /// plain guns, `None` when none answers.
    pub fn gun_range_stats<W: AiWorld + ?Sized>(
        &self,
        world: &W,
        ship: W::Handle,
    ) -> Option<(f32, f32)> {
        let mut count = 0_i32;
        let mut max = 0.0_f32;
        let mut sum = 0.0_f32;
        for slot in &self.guns {
            if slot.kind != kind::PLAIN {
                continue;
            }
            let Some(range) = world.equipment(ship, slot.id).and_then(|i| i.range) else {
                continue;
            };
            count += 1;
            sum = r(e(range) + e(sum));
            if range > max {
                max = range;
            }
        }
        (count > 0).then(|| (r(e(sum) / f64::from(count)), max))
    }

    /// `HasReadyWeaponSlot`: a gun of munition kind 0 to 2 that is neither
    /// destroyed nor disabled.
    #[must_use]
    pub fn has_ready_weapon_slot<W: AiWorld + ?Sized>(&self, world: &W, ship: W::Handle) -> bool {
        self.guns.iter().any(|slot| {
            world.equipment(ship, slot.id).is_some_and(|i| {
                i.munition_kind.is_some_and(|k| (0..=2).contains(&k))
                    && i.flags & (super::world::EQUIP_DESTROYED | super::world::EQUIP_DISABLED) == 0
            })
        })
    }

    /// `ScanAutoTurretTargets(dt)`: once a scan period, aim each auto
    /// turret at the nearest hostile of the attack order it can point at
    /// within 1.2 times the gun range. Returns the attack state to set (0
    /// with a hostile in the list, else 2), `None` when the scan did not
    /// run or the ship is of a ship class (`0x7f0000`).
    pub fn scan_auto_turret_targets<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        list: &mut super::target_manager::TargetList,
        dt: f32,
    ) -> Option<i32> {
        let sum = e(dt) + e(self.scan_timer);
        let timer = r(sum);
        self.scan_timer = timer;
        if !(sum > e(self.scan_period)) {
            return None;
        }
        self.scan_timer = r(e(timer) % e(self.scan_period));
        let range = e(self.gun_range) * 1.2;
        let range_sq = r(range * range);
        let own = world.inspect(ship)?;
        let (player_target, player_sub) = if own.is_player {
            (own.target, own.subtarget.unwrap_or(0))
        } else {
            (None, 0)
        };
        let self_may_engage = own.type_flags & 0xa0 == 0;
        let mut any_hostile = false;
        let guns = self.guns.clone();
        for slot in guns.iter().filter(|s| s.auto_turret) {
            let mut best_sq = f32::MAX;
            let mut best: Option<u32> = None;
            for (h, v) in list.views(&*world, ship) {
                if !matches_attack_order(p, v.type_flags) {
                    continue;
                }
                let Some((barrel, _)) = world.equipment(ship, slot.id).and_then(|i| i.barrel)
                else {
                    continue;
                };
                let dx = e(v.position.x) - e(barrel.x);
                let dy = e(v.position.y) - e(barrel.y);
                let dz = e(v.position.z) - e(barrel.z);
                let dist_sq = r((dz * dz + dy * dy) + dx * dx);
                // A ship whose manager was made by a content callback of
                // creation type 2 is spared by a ship that may not engage:
                // the creation type is not modelled, so none is.
                let _ = self_may_engage;
                let (attitude, _) = world.attitude_symmetrical(ship, h).unwrap_or((0.0, 0.0));
                if !(attitude <= -0.6) {
                    continue;
                }
                any_hostile = true;
                if dist_sq > range_sq {
                    continue;
                }
                if world.can_point_at(ship, slot.id, v.position, 0.3) && dist_sq < best_sq {
                    best_sq = dist_sq;
                    best = Some(v.id);
                }
            }
            let sub = if best.is_some() && best == player_target {
                player_sub
            } else {
                0
            };
            let target = best.and_then(|id| world.handle_of(id));
            world.command(
                ship,
                AiCommand::SetGunTarget {
                    equipment: slot.id,
                    target,
                    subtarget: sub,
                },
            );
        }
        if own.type_flags & 0x7f_0000 != 0 {
            return None;
        }
        Some(if any_hostile { 0 } else { 2 })
    }
}

/// Whether a view is an equipped object (`0x103`).
#[must_use]
pub fn is_eq_obj(v: &super::world::ObjectView) -> bool {
    v.is(class::EQ_OBJ)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::world::RecordedRand;

    #[test]
    fn slot_timing_draws_by_kind() {
        let g = GunGroups::default();
        let p = Personality::default();
        for (k, turret, draws) in [
            (kind::PLAIN, false, 4),
            (kind::PLAIN, true, 4),
            (kind::SEEKER, false, 2),
            (kind::HP_TYPE_10, false, 2),
            (kind::MINE_DROPPER, false, 2),
            (kind::CRUISE_DISRUPTOR, false, 1),
            (kind::UNCLASSIFIED, false, 1),
        ] {
            let mut slot = GunSlot {
                kind: k,
                auto_turret: turret,
                ..GunSlot::default()
            };
            let mut rand = RecordedRand::new(vec![16_384; draws]);
            g.randomize_slot_timing(&mut slot, &p, &mut rand);
            assert!(rand.exhausted_exactly(), "kind {k}");
            assert!(slot.phase <= 0.0 && slot.phase >= -slot.fire_interval);
        }
        // A mid draw leaves a plain gun's times at their means.
        let mut slot = GunSlot::default();
        let mut rand = RecordedRand::new(vec![16_384; 4]);
        g.randomize_slot_timing(&mut slot, &p, &mut rand);
        assert_eq!(slot.burst_time, 4.0);
        assert_eq!(slot.no_burst_time, 2.0);
        assert_eq!(slot.fire_interval, 0.5);
        assert_eq!(slot.phase, -0.25);
    }

    #[test]
    fn phase_steps_through_bursts() {
        let mut slot = GunSlot {
            kind: kind::PLAIN,
            burst_time: 1.0,
            no_burst_time: 0.5,
            fire_interval: 0.25,
            phase: 0.2,
            ..GunSlot::default()
        };
        slot.step(0.1);
        assert!(slot.ready());
        slot.step(0.1);
        assert!(!slot.fire_tick);
        slot.step(1.0);
        assert_eq!(slot.phase, -0.5);
        let mut seeker = GunSlot {
            kind: kind::SEEKER,
            phase: -1.0,
            ..GunSlot::default()
        };
        seeker.step(0.5);
        assert!(seeker.fire_tick);
        assert_eq!(seeker.phase, -0.5);
    }

    #[test]
    fn line_of_fire_cache_ages() {
        let mut g = GunGroups::default();
        g.line_of_fire[0] = LineOfFireSlot {
            in_use: true,
            needs_refresh: false,
            ..LineOfFireSlot::default()
        };
        g.step_timers(Some(true), 0.6);
        assert!(!g.line_of_fire[0].needs_refresh);
        g.step_timers(Some(true), 0.6);
        assert!(g.line_of_fire[0].needs_refresh);
        assert_eq!(g.cruise_held_time, 1.2);
        g.step_timers(None, 9.0);
        assert!(!g.line_of_fire[0].in_use);
        assert_eq!(g.cruise_held_time, 0.0);
    }
}
