//! The remaining equipment kinds: cargo stacks and pods
//! (`common_cecargo.cpp`), armor, lights, effects and repair droids
//! (`common_cesmallequip.cpp`, `common_cefx.cpp`), the scanner
//! (`common_cescanner.cpp`), the tractor (`common_cetractor.cpp`,
//! `common_tractorarm.cpp`) and the cloaking device (`common_cecloak.cpp`).

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

/// `TractorFailureCode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TractorFailure {
    /// No failure.
    #[default]
    None,
    /// The loot is out of range.
    OutOfRange,
    /// Something blocks the beam.
    Obstructed,
    /// The loot belongs to someone else.
    NotOwner,
    /// The tractor is busy.
    Busy,
}

/// `TractorArm::Mode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ArmMode {
    /// Off.
    #[default]
    Off,
    /// Reaching for the target.
    Reaching,
    /// Holding it at the tip.
    Holding,
}

/// `TractorArm` (0x24 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TractorArm {
    /// +0x04 the mode.
    pub mode: ArmMode,
    /// +0x08 the loot it pulls.
    pub target: Option<ObjHandle>,
    /// +0x1c the time left at the tip.
    pub timer: f32,
    /// +0x20 the last failure.
    pub error: TractorFailure,
}

/// `CETractor`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tractor {
    /// `max_length` (`GetRange`).
    pub max_length: f32,
    /// `reach_speed`.
    pub reach_speed: f32,
    /// +0x50 the arms.
    pub arms: Vec<TractorArm>,
}

impl Tractor {
    /// `CETractor::CETractor`.
    #[must_use]
    pub fn new(max_length: f32, reach_speed: f32) -> Self {
        Self {
            max_length,
            reach_speed,
            arms: Vec::new(),
        }
    }

    /// `CountActiveArms`.
    #[must_use]
    pub fn active_arms(&self) -> usize {
        self.arms.iter().filter(|a| a.mode != ArmMode::Off).count()
    }

    /// `AddTarget` after `VerifyTarget`: a new arm reaching for the loot,
    /// or the failure.
    pub fn add_target(&mut self, target: ObjHandle, verify: TractorFailure) -> TractorFailure {
        if verify != TractorFailure::None {
            return verify;
        }
        if self.arms.iter().any(|a| a.target == Some(target) && a.mode != ArmMode::Off) {
            return TractorFailure::None;
        }
        self.arms.push(TractorArm {
            mode: ArmMode::Reaching,
            target: Some(target),
            timer: 0.0,
            error: TractorFailure::None,
        });
        TractorFailure::None
    }

    /// `RemoveTarget`.
    pub fn remove_target(&mut self, target: ObjHandle) {
        for arm in &mut self.arms {
            if arm.target == Some(target) {
                arm.mode = ArmMode::Off;
                arm.target = None;
            }
        }
        self.arms.retain(|a| a.mode != ArmMode::Off);
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
