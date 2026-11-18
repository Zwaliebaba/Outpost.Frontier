//! What the AI and the client read about equipment, and the two sensors
//! that update with their owner: `IObjInspect::get_equipment_status`,
//! `get_equipment_val`, `can_point_at`, `is_pointing_at` and
//! `get_tgt_lead_fire_pos` (`Common/common_iobjinspectimpl.cpp`) over
//! `CEquip::GetStatus` and `CELauncher::GetStatus`; the scanner's scan
//! list (`CEScanner::Update`, `scan_for_types`, `ScanList::add_in_range`,
//! `add_obj`, `Common/common_cescanner.cpp`); the tractor's arms
//! (`CETractor::Update`, `AddTarget`, `RemoveTarget`, `VerifyTarget`,
//! `TractorArm::Update`, `common_cetractor.cpp`, `common_tractorarm.cpp`).

use neuron_core::math::{Transform, Vector, mul_transform};

use crate::equip::misc::{ArmStep, TractorFailure};
use crate::equip::{EquipEvent, EquipKind, SubObjId, types};
use crate::object::{KindData, ObjHandle, ObjectKind};

use super::{TickList, World};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `EquipStatus` (`include/common_iface.h`): the item's state bits and a
/// launcher's readiness bits.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct EquipStatus {
    /// +0x00 [`EquipStatus::ACTIVE`], [`EquipStatus::DESTROYED`],
    /// [`EquipStatus::DISABLED`].
    pub flags: u8,
    /// +0x04 [`EquipStatus::REFIRE_READY`], [`EquipStatus::NO_AMMO`],
    /// [`EquipStatus::POWER_OK`] (launchers).
    pub equip_flags: u32,
}

impl EquipStatus {
    /// `IsActive`.
    pub const ACTIVE: u8 = 0x01;
    /// `IsDestroyed`.
    pub const DESTROYED: u8 = 0x02;
    /// `IsDisabled`.
    pub const DISABLED: u8 = 0x04;
    /// `RefireDelayElapsed`.
    pub const REFIRE_READY: u32 = 0x02;
    /// `GetAmmoCount() == 0` (the other launcher bits are then not set).
    pub const NO_AMMO: u32 = 0x04;
    /// `PowerNeedsMet`.
    pub const POWER_OK: u32 = 0x08;
}

/// `EquipmentValType`: what `get_equipment_val` reads.
pub mod equipment_val {
    /// Power draw (guns, engines, thrusters), a power plant's charge rate.
    pub const POWER_DRAW: u32 = 0;
    /// Projectiles per fire times the munition's hull damage.
    pub const HULL_DAMAGE_PER_FIRE: u32 = 1;
    /// The gun's refire delay.
    pub const REFIRE_DELAY: u32 = 2;
    /// The ammunition count (`int`).
    pub const AMMO_COUNT: u32 = 3;
    /// A power plant's capacity.
    pub const CAPACITY: u32 = 4;
    /// The munition, radar or tractor range.
    pub const RANGE: u32 = 5;
    /// `int`: 3 cruise disruptor, 2 hardpoint type 10, else seeker != 0.
    pub const MUNITION_KIND: u32 = 6;
    /// The scanner's cargo scan range.
    pub const CARGO_SCAN_RANGE: u32 = 7;
    /// The gun's muzzle velocity.
    pub const MUZZLE_VELOCITY: u32 = 8;
    /// Byte: the gun's `auto_turret`.
    pub const AUTO_TURRET: u32 = 9;
    /// Byte: the munition has a seeker.
    pub const SEEKER: u32 = 0xa;
    /// `int`: the gun's locked target's id, or 0.
    pub const TARGET_ID: u32 = 0xb;
}

/// The value `get_equipment_val` writes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum EquipmentVal {
    /// A `float`.
    Float(f32),
    /// An `int`.
    Int(i32),
    /// One byte.
    Byte(u8),
}

/// The scanner's mask for its own update (`0x357ffdff`), and for a static
/// solar's (`0x157f0000`).
const SCAN_MASK: u32 = 0x357f_fdff;
const SCAN_MASK_STATIC_SOLAR: u32 = 0x157f_0000;
/// `ScanList`'s capacity.
const SCAN_CAPACITY: usize = 0x100;

impl World {
    /// `IObjInspect::get_equipment_status`: -3 for an object that is not
    /// equipped, -2 for a missing or destroyed item.
    pub fn equipment_status(&self, h: ObjHandle, id: SubObjId) -> Result<EquipStatus, i32> {
        let o = self.objects.get(h).ok_or(-3)?;
        let eq = o.equipped.as_deref().ok_or(-3)?;
        let equip = eq.equipment.by_id(id).ok_or(-2)?;
        if equip.is_destroyed() {
            return Err(-2);
        }
        let mut status = EquipStatus::default();
        if equip.is_active() {
            status.flags |= EquipStatus::ACTIVE;
        }
        if equip.is_disabled(eq.weapons_offline) {
            status.flags |= EquipStatus::DISABLED;
        }
        let launcher = match &equip.kind {
            EquipKind::Gun(g) => Some(&g.launcher),
            EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => Some(l),
            _ => None,
        };
        if let Some(l) = launcher
            && status.flags & (EquipStatus::DESTROYED | EquipStatus::DISABLED) == 0
        {
            let ammo = self.launch_context(h, id).map_or(-1, |cx| cx.ammo);
            if ammo == 0 {
                status.equip_flags |= EquipStatus::NO_AMMO;
                return Ok(status);
            }
            if l.power_needs_met(eq.power) {
                status.equip_flags |= EquipStatus::POWER_OK;
            }
            if l.refire_delay_elapsed() {
                status.equip_flags |= EquipStatus::REFIRE_READY;
            }
        }
        Ok(status)
    }

    /// `IObjInspect::get_equipment_val`: -3 for an object that is not
    /// equipped or a value the item has not, -2 for a missing or destroyed
    /// item, -4 for an engine throttle outside [-1, 1].
    pub fn equipment_val(
        &self,
        h: ObjHandle,
        id: SubObjId,
        kind: u32,
        param: f32,
    ) -> Result<EquipmentVal, i32> {
        use equipment_val as v;
        let o = self.objects.get(h).ok_or(-3)?;
        let eq = o.equipped.as_deref().ok_or(-3)?;
        let equip = eq.equipment.by_id(id).ok_or(-2)?;
        if equip.is_destroyed() {
            return Err(-2);
        }
        let functioning = equip.is_functioning(eq.weapons_offline);
        match &equip.kind {
            EquipKind::Power(p) => match kind {
                v::POWER_DRAW => Ok(EquipmentVal::Float(p.charge_rate)),
                v::CAPACITY => Ok(EquipmentVal::Float(p.capacity)),
                _ => Err(-3),
            },
            EquipKind::Engine(engine) if kind == v::POWER_DRAW => {
                let throttle = param.clamp(-1.0, 1.0);
                if !(throttle == param) {
                    return Err(-4);
                }
                Ok(EquipmentVal::Float(
                    engine.power_draw(throttle, functioning),
                ))
            }
            EquipKind::Thruster(t) if kind == v::POWER_DRAW => {
                Ok(EquipmentVal::Float(t.power_usage))
            }
            EquipKind::Gun(g) => {
                let ammo = g.launcher.projectile.unwrap_or_default();
                match kind {
                    v::POWER_DRAW => Ok(EquipmentVal::Float(r(g.launcher.power_draw_per_fire()))),
                    v::HULL_DAMAGE_PER_FIRE => Ok(EquipmentVal::Float(r(
                        g.launcher.barrel_count() as f64 * e(ammo.hull_damage),
                    ))),
                    v::REFIRE_DELAY => Ok(EquipmentVal::Float(g.launcher.refire_delay)),
                    v::AMMO_COUNT => Ok(EquipmentVal::Int(
                        self.launch_context(h, id).map_or(-1, |cx| cx.ammo),
                    )),
                    v::RANGE => {
                        let motor = self.data.archetypes.motors.get(&ammo.motor);
                        Ok(EquipmentVal::Float(g.munition_range(false, motor)))
                    }
                    v::MUNITION_KIND => Ok(EquipmentVal::Int(if ammo.cruise_disruptor {
                        3
                    } else if ammo.hp_type == 0xa {
                        2
                    } else {
                        i32::from(ammo.seeker != 0)
                    })),
                    v::MUZZLE_VELOCITY => Ok(EquipmentVal::Float(g.launcher.muzzle_velocity)),
                    v::AUTO_TURRET => Ok(EquipmentVal::Byte(u8::from(g.auto_turret))),
                    v::SEEKER => Ok(EquipmentVal::Byte(u8::from(ammo.seeker != 0))),
                    v::TARGET_ID => Ok(EquipmentVal::Int(
                        g.target
                            .and_then(|t| self.objects.get(t))
                            .map_or(0, |t| t.simple.id as i32),
                    )),
                    _ => Err(-3),
                }
            }
            EquipKind::Scanner(_) if kind == v::RANGE => {
                Ok(EquipmentVal::Float(self.radar_range(h, id).unwrap_or(0.0)))
            }
            EquipKind::Scanner(s) if kind == v::CARGO_SCAN_RANGE => {
                Ok(EquipmentVal::Float(s.cargo_scan_range))
            }
            EquipKind::Tractor(t) if kind == v::RANGE => Ok(EquipmentVal::Float(t.max_length)),
            _ => Err(-3),
        }
    }

    fn gun_inspect_check(&self, h: ObjHandle, id: SubObjId) -> Result<(), i32> {
        let o = self.objects.get(h).ok_or(-3)?;
        let eq = o.equipped.as_deref().ok_or(-3)?;
        let equip = eq.equipment.by_id(id).ok_or(-2)?;
        if equip.is_destroyed() {
            return Err(-2);
        }
        if equip.type_bits != types::GUN {
            return Err(-3);
        }
        Ok(())
    }

    /// `IObjInspect::can_point_at` ([`World::gun_can_point_at`]): -2 for a
    /// missing or destroyed item, -3 for one that is not a gun.
    pub fn can_point_at(
        &self,
        h: ObjHandle,
        id: SubObjId,
        point: Vector,
        angle: f32,
    ) -> Result<bool, i32> {
        self.gun_inspect_check(h, id)?;
        Ok(self.gun_can_point_at(h, id, point, angle))
    }

    /// `IObjInspect::is_pointing_at` ([`World::gun_can_see_target`]).
    pub fn is_pointing_at(&mut self, h: ObjHandle, id: SubObjId, angle: f32) -> Result<bool, i32> {
        self.gun_inspect_check(h, id)?;
        Ok(self.gun_can_see_target(h, id, angle))
    }

    /// `IObjInspect::get_tgt_lead_fire_pos` ([`World::gun_lead_position`]):
    /// -1 without a lead.
    pub fn tgt_lead_fire_pos(&mut self, h: ObjHandle, id: SubObjId) -> Result<Vector, i32> {
        self.gun_inspect_check(h, id)?;
        self.gun_lead_position(h, id).ok_or(-1)
    }

    // ---- scanner ------------------------------------------------------------------------

    /// `CEScanner::GetRadarRange`: the radar range less the owner's
    /// scanner interference.
    #[must_use]
    pub fn radar_range(&self, h: ObjHandle, id: SubObjId) -> Option<f32> {
        let o = self.objects.get(h)?;
        let equip = o.equipped.as_deref()?.equipment.by_id(id)?;
        let EquipKind::Scanner(s) = &equip.kind else {
            return None;
        };
        Some(r((1.0 - e(o.simple.scanner_interference)) * e(s.range)))
    }

    /// `ScanList::add_in_range` with `CEScanner::add_obj` as the filter.
    /// The solars of the system's spatial bucket come first (mask
    /// `0xfdff`, a sphere test), then the server's lists in `GetNext`'s
    /// order with the distance test.
    fn add_in_range(&mut self, h: ObjHandle, id: SubObjId, mask: u32, range: f32) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let Some(system) = o.simple.system else {
            return;
        };
        let pos = o.engine.position;
        let owner_id = o.simple.id;
        let owner_interference = o.simple.scanner_interference;
        let Some(arch_range) = o
            .equipped
            .as_deref()
            .and_then(|eq| eq.equipment.by_id(id))
            .and_then(|e| match &e.kind {
                EquipKind::Scanner(s) => Some(s.range),
                _ => None,
            })
        else {
            return;
        };
        let range_sq = r(e(range) * e(range));
        let list_system = o.physical.system;
        let _ = system;
        let accept = |w: &World, c: ObjHandle| -> bool {
            let Some(co) = w.objects.get(c) else {
                return false;
            };
            if co.simple.id == owner_id || !co.simple.targetable || co.dead {
                return false;
            }
            let target_interference = co.simple.scanner_interference;
            if !(e(owner_interference) < e(target_interference)) {
                return true;
            }
            let reach = r((1.0 - e(target_interference)) * e(arch_range));
            let p = co.engine.position;
            let dx = e(p.x) - e(pos.x);
            let dy = e(p.y) - e(pos.y);
            let dz = e(p.z) - e(pos.z);
            !(e(reach) * e(reach) < (dx * dx + dz * dz) + dy * dy)
        };
        let mut found: Vec<ObjHandle> = Vec::new();
        let full = |found: &Vec<ObjHandle>| found.len() >= SCAN_CAPACITY;
        if mask & 0xfdff != 0 {
            for c in self.list(list_system, TickList::Solars) {
                if full(&found) {
                    break;
                }
                let Some(co) = self.objects.get(c) else {
                    continue;
                };
                if co.simple.type_bits & 0x200 != 0 {
                    continue;
                }
                let p = co.engine.position;
                let reach = e(range) + e(co.engine.radius);
                let d = [e(p.x) - e(pos.x), e(p.y) - e(pos.y), e(p.z) - e(pos.z)];
                if d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > reach * reach {
                    continue;
                }
                if accept(self, c) {
                    found.push(c);
                }
            }
        }
        let walk = mask & 0xffff_0200;
        let blocks: [(u32, TickList, Option<u32>); 8] = [
            (0x007f_0000, TickList::Ships, Some(0x007f_0000)),
            (0xfdff, TickList::Solars, Some(0xfdff)),
            (0x1000_0000, TickList::Loot, None),
            (0x0100_0000, TickList::Guided, None),
            (0x0200_0000, TickList::Beams, None),
            (0x0400_0000, TickList::Mines, None),
            (0x0800_0000, TickList::CounterMeasures, None),
            (0x2000_0000, TickList::Asteroids, None),
        ];
        'walk: for (bits, list, filter) in blocks {
            if walk & bits == 0 {
                continue;
            }
            let filtered = filter.is_some_and(|f| walk & f != f);
            for c in self.list(list_system, list) {
                let Some(co) = self.objects.get(c) else {
                    continue;
                };
                if filtered && co.simple.type_bits & walk == 0 {
                    continue;
                }
                let p = co.engine.position;
                let dx = e(p.x) - e(pos.x);
                let dy = e(p.y) - e(pos.y);
                let dz = e(p.z) - e(pos.z);
                if !((dx * dx + dy * dy) + dz * dz < e(range_sq)) {
                    continue;
                }
                if full(&found) {
                    break 'walk;
                }
                if accept(self, c) {
                    found.push(c);
                }
            }
        }
        if let Some(EquipKind::Scanner(s)) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
            .and_then(|eq| eq.equipment.by_id_mut(id))
            .map(|e| &mut e.kind)
        {
            for c in found {
                if s.scans.len() < SCAN_CAPACITY {
                    s.scans.push(c);
                }
            }
        }
    }

    fn owner_is_static_solar(&self, h: ObjHandle) -> bool {
        self.objects
            .get(h)
            .is_some_and(|o| matches!(&o.data, KindData::Solar(s) if !s.dynamic))
    }

    fn scanner_scan(&mut self, h: ObjHandle, id: SubObjId, mask: u32) {
        let static_solar = self.owner_is_static_solar(h);
        let Some(o) = self.objects.get(h) else {
            return;
        };
        if o.simple.system.is_none() {
            return;
        }
        let Some(range) = self.radar_range(h, id) else {
            return;
        };
        let cached = match self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(id))
            .map(|e| &e.kind)
        {
            Some(EquipKind::Scanner(s)) => s.type_mask,
            _ => return,
        };
        let effective = if static_solar {
            mask & 0xdfff_0200
        } else {
            mask
        };
        let new_types = effective & !cached;
        self.add_in_range(h, id, new_types, range);
        if let Some(EquipKind::Scanner(s)) = self.scanner_mut(h, id) {
            s.type_mask |= new_types;
        }
    }

    fn scanner_mut(&mut self, h: ObjHandle, id: SubObjId) -> Option<&mut EquipKind> {
        self.objects
            .get_mut(h)?
            .equipped
            .as_deref_mut()?
            .equipment
            .by_id_mut(id)
            .map(|e| &mut e.kind)
    }

    fn scanner_functioning(&self, h: ObjHandle, id: SubObjId) -> bool {
        self.objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| {
                eq.equipment
                    .by_id(id)
                    .map(|e| e.is_functioning(eq.weapons_offline))
            })
            .unwrap_or(false)
    }

    /// `CEScanner::Update`: the list empties, then a functioning scanner
    /// lists what its owner can see (unless `flags` holds `NO_SCANNER`).
    pub(crate) fn update_scanner(&mut self, h: ObjHandle, id: SubObjId, flags: u32) {
        if let Some(EquipKind::Scanner(s)) = self.scanner_mut(h, id) {
            s.scans.clear();
            s.type_mask = 0;
        }
        if !self.scanner_functioning(h, id) || flags & crate::object::update_flags::NO_SCANNER != 0
        {
            return;
        }
        let mask = if self.owner_is_static_solar(h) {
            SCAN_MASK_STATIC_SOLAR
        } else {
            SCAN_MASK
        };
        let Some(range) = self.radar_range(h, id) else {
            return;
        };
        if self.objects.get(h).and_then(|o| o.simple.system).is_none() {
            return;
        }
        self.add_in_range(h, id, mask, range);
        if let Some(EquipKind::Scanner(s)) = self.scanner_mut(h, id) {
            s.type_mask |= mask;
        }
    }

    /// `CEScanner::scan_for_types`: adds the types of `mask` not scanned
    /// this frame to the first scanner's list.
    pub fn scan_for_types(&mut self, h: ObjHandle, mask: u32) {
        let Some(id) = self.first_of(h, types::SCANNER) else {
            return;
        };
        if !self.scanner_functioning(h, id) {
            return;
        }
        self.scanner_scan(h, id, mask);
    }

    /// `CEScanner::clear_cache`.
    pub fn scanner_clear_cache(&mut self, h: ObjHandle) {
        let Some(id) = self.first_of(h, types::SCANNER) else {
            return;
        };
        if let Some(EquipKind::Scanner(s)) = self.scanner_mut(h, id) {
            s.scans.clear();
            s.type_mask = 0;
        }
    }

    /// The first scanner's scan list: the objects it saw this frame that
    /// are still there, in scan order (what the AI calls `scan_list`).
    #[must_use]
    pub fn scan_list(&self, h: ObjHandle) -> Vec<ObjHandle> {
        let Some(eq) = self.objects.get(h).and_then(|o| o.equipped.as_deref()) else {
            return Vec::new();
        };
        match eq.equipment.first(types::SCANNER).map(|e| &e.kind) {
            Some(EquipKind::Scanner(s)) => s
                .scans
                .iter()
                .copied()
                .filter(|c| self.objects.contains(*c))
                .collect(),
            _ => Vec::new(),
        }
    }

    fn first_of(&self, h: ObjHandle, mask: u32) -> Option<SubObjId> {
        self.objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .first(mask)
            .map(|e| e.id)
    }

    // ---- tractor ------------------------------------------------------------------------

    /// `CETractor::GetSourcePos`: the owner's transform times the source
    /// hardpoint's.
    #[must_use]
    pub fn tractor_source_pos(&self, h: ObjHandle, id: SubObjId) -> Option<Vector> {
        let o = self.objects.get(h)?;
        let equip = o.equipped.as_deref()?.equipment.by_id(id)?;
        let EquipKind::Tractor(t) = &equip.kind else {
            return None;
        };
        let owner = Transform::new(o.engine.orientation, o.engine.position);
        Some(mul_transform(&owner, &t.hp_xform).translation)
    }

    /// The source hardpoint of a ship's tractor (`CETractor::CETractor`'s
    /// `FindHardpoint_PS` of the archetype's `HP_tractor_source`).
    pub(crate) fn init_tractor_source(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        if o.kind != ObjectKind::Ship {
            return;
        }
        let name = self
            .data
            .archetypes
            .ships
            .get(&o.archetype)
            .map(|s| self.data.strings.get(s.hp_tractor_source).to_vec())
            .unwrap_or_default();
        let Some((_, data)) = self.find_hardpoint(o.engine.instance, &name) else {
            return;
        };
        let xform = Transform::new(data.orientation(), data.position());
        if let Some(eq) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
        {
            for e in eq.equipment.iter_mut() {
                if let EquipKind::Tractor(t) = &mut e.kind {
                    t.hp_xform = xform;
                }
            }
        }
    }

    /// `CETractor::VerifyTarget`: loot with units, within range. The hold
    /// and capacity tests are not made (divergence register).
    #[must_use]
    pub fn tractor_verify_target(
        &self,
        h: ObjHandle,
        id: SubObjId,
        loot: Option<ObjHandle>,
    ) -> TractorFailure {
        let Some(l) = loot.and_then(|l| self.objects.get(l)) else {
            return TractorFailure::NoContents;
        };
        let KindData::Loot(data) = &l.data else {
            return TractorFailure::NoContents;
        };
        if data.units == 0 {
            return TractorFailure::NoContents;
        }
        let (Some(source), Some(t)) = (self.tractor_source_pos(h, id), self.tractor(h, id)) else {
            return TractorFailure::NoContents;
        };
        if !t.verify_range(source, l.engine.position) {
            return TractorFailure::OutOfRange;
        }
        TractorFailure::Ok
    }

    fn tractor(&self, h: ObjHandle, id: SubObjId) -> Option<&crate::equip::misc::Tractor> {
        match &self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?
            .kind
        {
            EquipKind::Tractor(t) => Some(t),
            _ => None,
        }
    }

    /// `CETractor::AddTarget`: an arm reaches for the loot and the tractor
    /// activates; a refusal is reported (`Event_0x14`) and returned.
    pub fn tractor_add_target(
        &mut self,
        h: ObjHandle,
        id: SubObjId,
        loot: ObjHandle,
    ) -> TractorFailure {
        let code = self.tractor_verify_target(h, id, Some(loot));
        let Some(equip) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
            .and_then(|eq| eq.equipment.by_id_mut(id))
        else {
            return code;
        };
        if code != TractorFailure::Ok {
            equip.notify(EquipEvent::TractorRefused(code));
            return code;
        }
        if let EquipKind::Tractor(t) = &mut equip.kind {
            t.add_target(loot);
        }
        if !equip.is_active() {
            equip.activate_base(true);
        }
        TractorFailure::Ok
    }

    /// `CETractor::RemoveTarget`: the arms on the loot with that id turn
    /// off; the tractor deactivates when none is left.
    pub fn tractor_remove_target(&mut self, h: ObjHandle, id: SubObjId, loot_id: u32) {
        let ids: Vec<(ObjHandle, u32)> = self
            .tractor(h, id)
            .map(|t| {
                t.arms
                    .iter()
                    .filter_map(|a| a.target)
                    .filter_map(|l| self.objects.get(l).map(|o| (l, o.simple.id)))
                    .collect()
            })
            .unwrap_or_default();
        let Some(equip) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
            .and_then(|eq| eq.equipment.by_id_mut(id))
        else {
            return;
        };
        let EquipKind::Tractor(t) = &mut equip.kind else {
            return;
        };
        let remaining = t.remove_target(|target| {
            target.is_some_and(|l| ids.iter().any(|&(h, i)| h == l && i == loot_id))
        });
        if remaining == 0 {
            equip.activate_base(false);
        }
    }

    /// `CETractor::Update`: a functioning tractor updates each arm
    /// (`TractorArm::Update`: a target that fails `VerifyTarget` turns the
    /// arm off) and deactivates when no arm is left.
    pub(crate) fn update_tractor(&mut self, h: ObjHandle, id: SubObjId, dt: f32) {
        let functioning = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| {
                eq.equipment
                    .by_id(id)
                    .map(|e| e.is_functioning(eq.weapons_offline))
            })
            .unwrap_or(false);
        if !functioning {
            return;
        }
        let count = self.tractor(h, id).map_or(0, |t| t.arms.len());
        let mut active = 0;
        for i in 0..count {
            let Some(arm) = self.tractor(h, id).map(|t| t.arms[i]) else {
                return;
            };
            if arm.mode != crate::equip::misc::ArmMode::Off {
                let code = self.tractor_verify_target(h, id, arm.target);
                let source = self.tractor_source_pos(h, id).unwrap_or(Vector::ZERO);
                let target_pos = arm
                    .target
                    .and_then(|l| self.objects.get(l))
                    .map(|o| o.engine.position);
                if let Some(equip) = self
                    .objects
                    .get_mut(h)
                    .and_then(|o| o.equipped.as_deref_mut())
                    .and_then(|eq| eq.equipment.by_id_mut(id))
                {
                    let crate::equip::Equip { kind, events, .. } = equip;
                    if let EquipKind::Tractor(t) = kind {
                        let reach = t.reach_speed;
                        let a = &mut t.arms[i];
                        if code == TractorFailure::Ok {
                            let target = a.target;
                            match a.step(source, target_pos.unwrap_or(source), reach, dt) {
                                ArmStep::Reached => events.push(EquipEvent::TractorReached(target)),
                                ArmStep::Consume => events.push(EquipEvent::TractorConsume(target)),
                                ArmStep::Idle | ArmStep::Pulled => {}
                            }
                        } else {
                            a.error = code;
                            events.push(EquipEvent::TractorArmLost(code));
                            a.turn_off();
                        }
                    }
                }
            }
            if self
                .tractor(h, id)
                .is_some_and(|t| t.arms[i].mode != crate::equip::misc::ArmMode::Off)
            {
                active += 1;
            }
        }
        if active == 0
            && let Some(equip) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
                .and_then(|eq| eq.equipment.by_id_mut(id))
        {
            equip.activate_base(false);
        }
    }
}
