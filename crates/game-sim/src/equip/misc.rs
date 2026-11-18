//! The remaining equipment kinds: cargo stacks and pods
//! (`common_cecargo.cpp`), armor, lights, effects and repair droids
//! (`common_cesmallequip.cpp`, `common_cefx.cpp`), the scanner
//! (`common_cescanner.cpp`), the tractor (`common_cetractor.cpp`,
//! `common_tractorarm.cpp`) and the cloaking device (`common_cecloak.cpp`).

use neuron_core::math::{Matrix, Transform, Vector};

use crate::object::ObjHandle;

use super::SubObjId;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `CECargo`: a stack of one archetype in the hold or a pod.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Cargo {
    /// +0x20 the pod holding the stack (its id).
    pub pod: Option<SubObjId>,
    /// +0x24 the hit points of the stack's top unit.
    pub hit_pts: f32,
    /// +0x28 the count.
    pub count: u32,
    /// The commodity's `decay_per_second` (0 for any other kind).
    pub decay_per_second: f32,
}

impl Cargo {
    /// `CECargo::CECargo`: empty.
    #[must_use]
    pub fn new(decay_per_second: f32) -> Self {
        Self {
            decay_per_second,
            ..Self::default()
        }
    }

    /// `Init`'s numbers (the pod link is the owner's).
    pub fn init(&mut self, health: f32, count: u32, max_hit_pts: f32) {
        self.hit_pts = r(e(max_hit_pts) * e(health));
        self.count = count;
    }

    /// `CECargo::IsDestroyed`: an empty stack is destroyed.
    #[must_use]
    pub fn is_destroyed(&self, flag: bool) -> bool {
        if self.count != 0 { flag } else { true }
    }

    /// `GetMaxHitPoints`: the archetype's.
    #[must_use]
    pub fn max_hit_points(&self, arch_hit_pts: f32) -> f32 {
        arch_hit_pts
    }

    fn relative_health(&self, max: f32) -> f64 {
        if max == 0.0 {
            return 0.0;
        }
        e(self.hit_pts) / e(max)
    }

    /// `AddToStack(amount, health)`.
    pub fn add_to_stack(&mut self, amount: u32, health: f32, max: f32) {
        let mut health = health;
        if !(self.relative_health(max) < 1.0) {
            self.count = self.count.wrapping_add(amount);
        } else {
            if !(health < 1.0) {
                self.count = self.count.wrapping_add(amount);
                return;
            }
            health = r(self.relative_health(max) + e(health));
            if !(health < 1.0) {
                self.count = self.count.wrapping_add(amount);
                health -= 1.0;
            } else {
                self.count = self.count.wrapping_add(amount.wrapping_sub(1));
            }
        }
        self.hit_pts = r(e(max) * e(health));
    }

    /// `RemoveFromStack(amount)`: the top unit is whole again.
    pub fn remove_from_stack(&mut self, amount: u32, max: f32) {
        self.count = self.count.saturating_sub(amount);
        self.hit_pts = max;
    }
}

/// `CECargoPod`: an external pod; the stack it holds.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CargoPod {
    /// The stack in it (`FillPod` / `EmptyPod`).
    pub contents: Option<SubObjId>,
}

/// `CEArmor`: divides hull and group damage (`hit_pts_scale`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Armor {
    /// +0x70 `hit_pts_scale`.
    pub hit_pts_scale: f32,
}

/// `CELightEquip`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Light {
    /// `always_on`.
    pub always_on: bool,
    /// `docking_light`.
    pub docking_light: bool,
    /// Lit.
    pub on: bool,
}

/// `CERepairDroid`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RepairDroid {
    /// `repair_rate`.
    pub repair_rate: f32,
}

/// `CEScanner::ScanResult`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScanResult {
    /// 0.
    TooFar,
    /// 1.
    NotEnoughPower,
    /// 2.
    Complete,
    /// 4.
    NoTarget,
}

/// `CEScanner`: the radar and cargo scan ranges, and the scan list the
/// world fills (`ScanList`, objects in range of the type mask).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Scanner {
    /// `power_usage`.
    pub power_usage: f32,
    /// `range` (`GetRadarRange`).
    pub range: f32,
    /// `cargo_scan_range`.
    pub cargo_scan_range: f32,
    /// +0x83c the type mask of the last scan.
    pub type_mask: u32,
    /// The objects the last scan found, in scan order.
    pub scans: Vec<ObjHandle>,
}

impl Scanner {
    /// `CEScanner::CEScanner`.
    #[must_use]
    pub fn new(power_usage: f32, range: f32, cargo_scan_range: f32) -> Self {
        Self {
            power_usage,
            range,
            cargo_scan_range,
            ..Self::default()
        }
    }

    /// `ScanCargo`'s range and power test: `distance_sq` from the owner to
    /// the target, the owner's power.
    #[must_use]
    pub fn scan_cargo(&self, target: bool, distance_sq: f64, power: f32) -> ScanResult {
        if !target {
            return ScanResult::NoTarget;
        }
        if !(distance_sq <= e(self.cargo_scan_range) * e(self.cargo_scan_range)) {
            return ScanResult::TooFar;
        }
        if power < self.power_usage {
            return ScanResult::NotEnoughPower;
        }
        ScanResult::Complete
    }
}

/// `TractorFailureCode` (`include/common_cequip.h`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum TractorFailure {
    /// 0: no failure.
    #[default]
    Ok,
    /// 1: no loot, or loot with no units.
    NoContents,
    /// 2: beyond the tractor's `max_length`.
    OutOfRange,
    /// 3: the hold cannot take the contents' volume.
    CargoFull,
    /// 4: no room for another shield battery.
    ShieldBattFull,
    /// 5: no room for another repair kit.
    RepairKitFull,
    /// 6: no room for the ammunition.
    AmmoFull,
}

impl TractorFailure {
    /// The reference's code.
    #[must_use]
    pub fn code(self) -> i32 {
        match self {
            Self::Ok => 0,
            Self::NoContents => 1,
            Self::OutOfRange => 2,
            Self::CargoFull => 3,
            Self::ShieldBattFull => 4,
            Self::RepairKitFull => 5,
            Self::AmmoFull => 6,
        }
    }
}

/// `TractorArm::Mode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ArmMode {
    /// 0: off.
    #[default]
    Off,
    /// 1: pulling the beam's tip towards the loot.
    Reaching,
    /// 2: the tip holds the loot; the consume timer runs.
    Holding,
}

/// `TractorArm::ACQUIRE_DELAY`: the time the tip holds the loot before it
/// is consumed.
pub const ACQUIRE_DELAY: f32 = 1.0;

/// `TractorArm` (0x24 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TractorArm {
    /// +0x04 the mode.
    pub mode: ArmMode,
    /// +0x08 the loot it pulls (`TargetWatch`).
    pub target: Option<ObjHandle>,
    /// +0x10 the tip's offset from the beam source.
    pub pull_offset: Vector,
    /// +0x1c the time left at the tip.
    pub timer: f32,
    /// +0x20 the last failure.
    pub error: TractorFailure,
}

/// What one arm's update does (`TractorArm::Update` past `VerifyTarget`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArmStep {
    /// Nothing happened (off, or holding with time left).
    Idle,
    /// The tip moved towards the loot.
    Pulled,
    /// The tip reached the loot (`Event_0x17`); the arm holds it now.
    Reached,
    /// The hold time ran out (`Event_0x18`).
    Consume,
}

impl TractorArm {
    /// `TractorArm::SetTarget`.
    pub fn set_target(&mut self, target: ObjHandle) {
        self.target = Some(target);
        self.pull_offset = Vector::ZERO;
        self.timer = 0.0;
        self.error = TractorFailure::Ok;
        self.mode = ArmMode::Reaching;
    }

    /// `TractorArm::TurnOff`.
    pub fn turn_off(&mut self) {
        self.target = None;
        self.mode = ArmMode::Off;
        self.pull_offset = Vector::ZERO;
        self.timer = 0.0;
        self.error = TractorFailure::Ok;
    }

    /// `TractorArm::Update` once the target verified: the tip moves from
    /// the beam source towards the loot at `reach_speed`, snaps onto it
    /// when this frame's step reaches it, then counts the hold time down.
    pub fn step(
        &mut self,
        source: Vector,
        target_pos: Vector,
        reach_speed: f32,
        dt: f32,
    ) -> ArmStep {
        match self.mode {
            ArmMode::Off => ArmStep::Idle,
            ArmMode::Reaching => {
                let tip = Vector::new(
                    r(e(source.x) + e(self.pull_offset.x)),
                    r(e(source.y) + e(self.pull_offset.y)),
                    r(e(source.z) + e(self.pull_offset.z)),
                );
                let dx = e(r(e(target_pos.x) - e(tip.x)));
                let dy = e(target_pos.y) - e(tip.y);
                let dz = e(r(e(target_pos.z) - e(tip.z)));
                let max_step = e(dt) * e(reach_speed);
                let dist_sq = dz * dz + dy * dy + dx * dx;
                if dist_sq > max_step * max_step {
                    let frac = max_step / dist_sq.sqrt();
                    let step_y = r(dy * frac);
                    let step_z = r(dz * frac);
                    self.pull_offset = Vector::new(
                        r(dx * frac + e(self.pull_offset.x)),
                        r(e(step_y) + e(self.pull_offset.y)),
                        r(e(step_z) + e(self.pull_offset.z)),
                    );
                    return ArmStep::Pulled;
                }
                self.pull_offset = Vector::new(
                    r(e(target_pos.x) - e(source.x)),
                    r(e(target_pos.y) - e(source.y)),
                    r(e(target_pos.z) - e(source.z)),
                );
                self.mode = ArmMode::Holding;
                self.timer = ACQUIRE_DELAY;
                ArmStep::Reached
            }
            ArmMode::Holding => {
                self.timer = r(e(self.timer) - e(dt));
                if self.timer < 0.0 {
                    ArmStep::Consume
                } else {
                    ArmStep::Idle
                }
            }
        }
    }

    /// `TractorArm::GetTipPos`: the source plus the offset while reaching,
    /// the loot's position while holding, else the source.
    #[must_use]
    pub fn tip_pos(&self, source: Vector, target_pos: Option<Vector>) -> Vector {
        match self.mode {
            ArmMode::Reaching => Vector::new(
                r(e(source.x) + e(self.pull_offset.x)),
                r(e(source.y) + e(self.pull_offset.y)),
                r(e(source.z) + e(self.pull_offset.z)),
            ),
            ArmMode::Holding => target_pos.unwrap_or(source),
            ArmMode::Off => source,
        }
    }
}

/// `CETractor`.
#[derive(Clone, Debug, PartialEq)]
pub struct Tractor {
    /// `max_length` (`GetRange`).
    pub max_length: f32,
    /// `reach_speed`.
    pub reach_speed: f32,
    /// +0x20 the beam source in the owner's frame (`HP_tractor_source`,
    /// identity when the ship has none).
    pub hp_xform: Transform,
    /// +0x50 the arms.
    pub arms: Vec<TractorArm>,
}

impl Default for Tractor {
    fn default() -> Self {
        Self::new(0.0, 0.0)
    }
}

impl Tractor {
    /// `CETractor::CETractor` (the world sets the source hardpoint).
    #[must_use]
    pub fn new(max_length: f32, reach_speed: f32) -> Self {
        Self {
            max_length,
            reach_speed,
            hp_xform: Transform::new(Matrix::IDENTITY, Vector::ZERO),
            arms: Vec::new(),
        }
    }

    /// `CountActiveArms`.
    #[must_use]
    pub fn active_arms(&self) -> usize {
        self.arms.iter().filter(|a| a.mode != ArmMode::Off).count()
    }

    /// `AddTarget` after `VerifyTarget` passed: the first idle arm, or a
    /// new one, reaches for the loot.
    pub fn add_target(&mut self, target: ObjHandle) {
        let slot = match self.arms.iter().position(|a| a.mode == ArmMode::Off) {
            Some(i) => i,
            None => {
                self.arms.push(TractorArm::default());
                self.arms.len() - 1
            }
        };
        self.arms[slot].set_target(target);
    }

    /// `RemoveTarget`: the arms on which `is_target` holds turn off; the
    /// number of arms still active (the caller deactivates the tractor at
    /// 0).
    pub fn remove_target(&mut self, is_target: impl Fn(Option<ObjHandle>) -> bool) -> usize {
        let mut remaining = 0;
        for arm in &mut self.arms {
            if arm.mode == ArmMode::Off {
                continue;
            }
            if is_target(arm.target) {
                arm.turn_off();
            } else {
                remaining += 1;
            }
        }
        remaining
    }

    /// `CanSeeTarget`: the loot is within range (a range of exactly 0
    /// reaches everything).
    #[must_use]
    pub fn can_see(&self, source: Vector, target_pos: Vector) -> bool {
        let dx = e(target_pos.x) - e(source.x);
        let dy = e(target_pos.y) - e(source.y);
        let dz = e(target_pos.z) - e(source.z);
        let range = self.max_length;
        !(range != 0.0 && e(range) * e(range) < dz * dz + dy * dy + dx * dx)
    }

    /// `VerifyTarget`'s range test, on the `float` differences.
    #[must_use]
    pub fn verify_range(&self, source: Vector, target_pos: Vector) -> bool {
        let dx = e(r(e(target_pos.x) - e(source.x)));
        let dy = e(r(e(target_pos.y) - e(source.y)));
        let dz = e(r(e(target_pos.z) - e(source.z)));
        let range = self.max_length;
        !(range != 0.0 && e(range) * e(range) < dy * dy + dz * dz + dx * dx)
    }
}

/// `CECloakingDevice`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Cloak {
    /// `power_usage`.
    pub power_usage: f32,
    /// `cloakin_time`.
    pub cloak_in_time: f32,
    /// `cloakout_time`.
    pub cloak_out_time: f32,
    /// The fade timer.
    pub timer: f32,
    /// The state being faded to (1 cloaked).
    pub fade_target: bool,
    /// The state reached.
    pub baseline: bool,
}

impl Cloak {
    /// `CECloakingDevice::CECloakingDevice` before its `Activate(true)`.
    #[must_use]
    pub fn new(power_usage: f32, cloak_in_time: f32, cloak_out_time: f32) -> Self {
        Self {
            power_usage,
            cloak_in_time,
            cloak_out_time,
            ..Self::default()
        }
    }

    /// `cloak_percent`, unrounded.
    #[must_use]
    pub fn cloak_percent(&self) -> f64 {
        if self.fade_target == self.baseline {
            return if self.baseline { 1.0 } else { 0.0 };
        }
        if self.fade_target {
            if e(self.cloak_out_time) > 0.001 {
                return e(self.timer) / e(self.cloak_out_time);
            }
            return 1.0;
        }
        let mut frac = 0.0;
        if e(self.cloak_in_time) > 0.001 {
            frac = e(self.timer) / e(self.cloak_in_time);
        }
        1.0 - frac
    }

    /// `CECloakingDevice::Activate`'s cloak part, after the equipment's
    /// active flag became `active`.
    pub fn activate(&mut self, active: bool) {
        let old = self.fade_target;
        if old == active {
            return;
        }
        if old == self.baseline {
            self.fade_target = active;
            self.timer = 0.0;
            return;
        }
        let frac = self.cloak_percent();
        self.timer = if old {
            r((1.0 - frac) * e(self.cloak_in_time))
        } else {
            r(frac * e(self.cloak_out_time))
        };
        self.fade_target = active;
    }

    /// `CECloakingDevice::Update`'s fade: returns the owner's new cloak
    /// percentage while fading.
    pub fn update(&mut self, destroyed: bool, dt: f32) -> Option<f32> {
        if destroyed || self.fade_target == self.baseline {
            return None;
        }
        let total = if self.fade_target {
            self.cloak_out_time
        } else {
            self.cloak_in_time
        };
        let elapsed = e(dt) + e(self.timer);
        self.timer = r(elapsed);
        if elapsed > e(total) {
            self.baseline = self.fade_target;
            self.timer = 0.0;
        }
        Some(r(self.cloak_percent()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cargo_stacks_merge_health() {
        let mut c = Cargo::new(0.0);
        c.init(0.5, 3, 10.0);
        assert_eq!(c.hit_pts, 5.0);
        c.add_to_stack(2, 0.75, 10.0);
        // 0.5 + 0.75 = 1.25: two whole units more, the top at a quarter.
        assert_eq!(c.count, 5);
        assert_eq!(c.hit_pts, 2.5);
        c.remove_from_stack(9, 10.0);
        assert_eq!(c.count, 0);
        assert!(c.is_destroyed(false));
    }

    #[test]
    fn cloak_fades_in_and_out() {
        let mut c = Cloak::new(1.0, 2.0, 4.0);
        c.activate(true);
        c.timer = c.cloak_out_time;
        assert_eq!(c.update(false, 0.5), Some(1.0));
        assert!(c.baseline);
        c.activate(false);
        assert_eq!(c.update(false, 1.0), Some(0.5));
        assert_eq!(c.update(false, 1.5), Some(0.0));
    }

    #[test]
    fn scanner_checks_range_then_power() {
        let s = Scanner::new(10.0, 2000.0, 100.0);
        assert_eq!(s.scan_cargo(false, 0.0, 50.0), ScanResult::NoTarget);
        assert_eq!(s.scan_cargo(true, 20000.0, 50.0), ScanResult::TooFar);
        assert_eq!(s.scan_cargo(true, 100.0, 5.0), ScanResult::NotEnoughPower);
        assert_eq!(s.scan_cargo(true, 100.0, 50.0), ScanResult::Complete);
    }
}
