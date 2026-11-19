//! Free flight transliterated from IVP (decision O19): the core state, the
//! PSI clock and the per-PSI sequence of `IVP_Environment_Simulate_Psi`
//! (`CppSource/IVP/ivp_physics/ivp_environment.cpp:661-720`).
//!
//! One PSI is:
//!
//! 1. the wake queue: frozen cores that something pushed are revived
//!    ([`revive_core`]);
//! 2. per simulation unit, in [`unit_psi_begin`]: the core step (matrix
//!    and position at the start of the PSI, commit of pending changes) and
//!    the unit's slow and fast bits; then the movement-check counter
//!    ([`MovementCheck`]); then the controllers from the highest priority
//!    down: the game's force controllers (1500, `push_core_linear`,
//!    `push_core_angular_ws`), the standard controller (1000, damping and
//!    commit, [`core::Core::gravity_controller_step`]) and golems (500);
//!    then [`unit_psi_end`], which may put the unit to sleep;
//! 3. `calc_next_PSI_matrix` for every core simulated in this PSI
//!    ([`psi::calc_next_psi_matrix`]).
//!
//! [`PsiClock`] is the environment's time manager reduced to the one event
//! it schedules here, the PSI: each [`PsiClock::simulate_dtime`] runs
//! exactly one PSI whose length is the step clamped to [0.005, 0.1] s
//! (fact F16).

pub mod core;
pub mod math;
pub mod psi;

use self::core::{Core, MOVEMENT_FROZEN};
use self::math::{IvpTime, e, r};

/// The PSI length of a new environment: `1 / PHYSICAL_SIM_RATE`
/// (`PhysicalSystem::AddRef`). Every [`PsiClock::simulate_dtime`] replaces
/// it with its own step.
#[must_use]
pub fn initial_psi_time(physical_sim_rate: f32) -> f32 {
    r(1.0 / e(physical_sim_rate))
}

/// `IVP_ENVIRONMENT_DEFAULT_DELTA_PSI_TIME` (`1.0f / 66.0f`).
pub const DEFAULT_DELTA_PSI_TIME: f32 = 1.0 / 66.0;

/// The environment's clock: `current_time`, the PSI bookkeeping of
/// `IVP_Time_Event_PSI::simulate_time_event` and the event manager's time
/// base, with the PSI as the only queued event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PsiClock {
    /// `current_time`.
    pub current_time: IvpTime,
    /// `time_of_last_psi`: the start of the PSI last run.
    pub time_of_last_psi: IvpTime,
    /// `next_psi_time`.
    pub next_psi_time: IvpTime,
    /// `delta_time`: the length of the PSI last run.
    pub delta_time: f32,
    /// `inv_delta_time`.
    pub inv_delta_time: f32,
    /// The time manager's base (`TimeBaseHi`, `TimeBaseLo`).
    pub base: IvpTime,
    /// The PSI event's key relative to [`PsiClock::base`].
    pub psi_key: f32,
}

impl PsiClock {
    /// A new environment's clock: time zero, the PSI queued at key 0.
    #[must_use]
    pub fn new(psi_time: f32) -> Self {
        Self {
            current_time: IvpTime::default(),
            time_of_last_psi: IvpTime::default(),
            // `IVP_Environment`'s constructor splits its default PSI time
            // (`IVP_ENVIRONMENT_DEFAULT_DELTA_PSI_TIME`, 1/66) into the next
            // PSI's time; `set_delta_PSI_time` does not move it, so a core
            // revived before the first PSI synchronises to 1/66.
            next_psi_time: IvpTime::new(0.0, DEFAULT_DELTA_PSI_TIME),
            delta_time: psi_time,
            inv_delta_time: r(1.0 / e(psi_time)),
            base: IvpTime::default(),
            psi_key: 0.0,
        }
    }

    /// `IVP_Sim_Unit_Manager::simulate_variable_time_step`: clamp the step,
    /// then run the queued PSI with that length through `psi` and stop at
    /// the next one. The clock's `current_time` ends at the next PSI.
    pub fn simulate_dtime(&mut self, delta_time: f32, psi: impl FnOnce(&mut PsiClock)) {
        let dt = if !(delta_time >= 0.005) {
            0.005
        } else if delta_time > 0.1 {
            0.1
        } else {
            delta_time
        };
        self.current_time =
            IvpTime::normalized(e(self.base.base), e(self.psi_key) + e(self.base.offset));
        self.delta_time = dt;
        self.inv_delta_time = r(1.0 / e(dt));
        self.time_of_last_psi = self.current_time;
        self.next_psi_time = IvpTime::normalized(
            e(self.time_of_last_psi.base),
            e(self.delta_time) + e(self.time_of_last_psi.offset),
        );
        self.base = self.time_of_last_psi;
        psi(self);
        self.psi_key = r(((e(self.next_psi_time.base) - e(self.base.base))
            + e(self.next_psi_time.offset))
            - e(self.base.offset));
        self.current_time =
            IvpTime::normalized(e(self.base.base), e(self.psi_key) + e(self.base.offset));
    }

    /// The [`psi::EventSim`] the controllers receive
    /// (`simulate_sim_units_psi`).
    #[must_use]
    pub fn event_sim(&self) -> psi::EventSim {
        psi::EventSim {
            delta_time: self.delta_time,
            i_delta_time: self.inv_delta_time,
        }
    }

    /// The [`psi::EventSim`] `commit_all_calc_next_PSI_matrix` builds.
    #[must_use]
    pub fn commit_event_sim(&self) -> psi::EventSim {
        let dt = e(self.delta_time);
        if dt > e(f32::from_bits(0x2edb_e6ff)) {
            psi::EventSim {
                delta_time: self.delta_time,
                i_delta_time: r(1.0 / dt),
            }
        } else {
            self.event_sim()
        }
    }

    /// The values `set_transformation` reads.
    #[must_use]
    pub fn beam_clock(&self) -> psi::BeamClock {
        psi::BeamClock {
            current_time: self.current_time,
            time_of_last_psi: self.time_of_last_psi,
            delta_time: self.delta_time,
            inv_delta_time: self.inv_delta_time,
        }
    }

    /// The `{dt, 1/dt}` pair a new or revived core synchronises to: the
    /// time to the next PSI when it is above 1e-10, else the PSI length
    /// (`ivp_real_object.cpp:2530`, `revive_pass`).
    #[must_use]
    pub fn time_to_next_psi(&self) -> (f32, f32) {
        let alpha = self.next_psi_time.since(self.current_time);
        if alpha > e(f32::from_bits(0x2edb_e6ff)) {
            (r(alpha), r(1.0 / alpha))
        } else {
            (self.delta_time, self.inv_delta_time)
        }
    }
}

/// `IVP_FastRandomFloat` (`ivp_random.cpp`): one process-wide 32-bit seed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IvpRandom {
    /// The seed, 1 at start.
    pub seed: u32,
}

impl Default for IvpRandom {
    fn default() -> Self {
        Self { seed: 1 }
    }
}

impl IvpRandom {
    /// The next value in `[0, 1)`: `seed *= 75`, the low 16 bits / 65536.
    pub fn next_float(&mut self) -> f32 {
        self.seed = self.seed.wrapping_mul(3).wrapping_mul(5).wrapping_mul(5);
        (self.seed & 0xffff) as f32 * (1.0 / 65536.0)
    }
}

/// The environment's movement-check countdown (`next_movement_check`,
/// 10 at start) that every simulation unit decrements once per PSI; the
/// unit that brings it to zero classifies its cores' motion.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MovementCheck {
    /// The countdown.
    pub counter: i16,
}

impl Default for MovementCheck {
    fn default() -> Self {
        Self { counter: 10 }
    }
}

impl MovementCheck {
    /// One unit's decrement; on zero the counter is reseeded to
    /// `15 - (int)(random * -5)` and the unit checks its motion.
    pub fn tick(&mut self, random: &mut IvpRandom) -> bool {
        self.counter = self.counter.wrapping_sub(1);
        if self.counter != 0 {
            return false;
        }
        let value = e(random.next_float()) * e(-5.0f32);
        let truncated = crate::num::trunc_to_i32(value);
        self.counter = (0xF - truncated) as i16;
        true
    }
}

/// A simulation unit's flag word: low byte 1 while simulated, 8 asleep;
/// bits 0x400 and 0x800 mark a fast or slow last PSI and 0x1000 and 0x2000
/// carry the previous PSI's.
pub const UNIT_ACTIVE: u32 = 1;

/// A simulation unit's flag bits that mark a changed controller set
/// (0x100, 0x200); the unit clears them at the end of its PSI.
pub const UNIT_TOPOLOGY_CHANGED: u32 = 0x300;

/// The core part of `simulate_single_sim_unit_psi` for a unit, up to the
/// controllers: the core step, then the unit's fast and slow bits. A slow
/// PSI after a fast one restarts the core's motion clock.
pub fn unit_psi_begin(core: &mut Core, unit_state: &mut u32, now: IvpTime) {
    let verdict = core.psi_core_step(now);
    if (verdict as i32) < 0 {
        *unit_state = (*unit_state & 0xFFFF_F7FF) | 0x400;
    } else {
        let mut state = *unit_state;
        let shifted = (((state << 0x14) as i32) >> 0x12) as u32;
        state ^= (shifted ^ state) & 0x3000;
        *unit_state = state;
        if state & 0x3000 != 0 {
            core.update_time_of_last_motion(now);
        }
        *unit_state &= 0xFFFF_F3FF;
    }
}

/// The rest of the unit's PSI after the controllers: when the movement
/// check came to this unit, classify the motion and put the unit to sleep
/// when it has been idle long enough (`IVP_Simulation_Unit_Update_Motion_And_Maybe_Sleep`).
/// Returns true when the unit went to sleep.
pub fn unit_psi_end(
    core: &mut Core,
    unit_state: &mut u32,
    now: IvpTime,
    motion_check: bool,
    elapsed_limit: f32,
) -> bool {
    let slept = motion_check && {
        let result = core.classify_motion_state(now, elapsed_limit);
        core.movement_state = (core.movement_state & !0xFF) | (result as u32 & 0xFF);
        if result & 3 == 3 {
            core.stop_movement();
            *unit_state = (*unit_state & 0xFFFF_FF08) | MOVEMENT_FROZEN;
            true
        } else {
            false
        }
    };
    // The unit's tail: when a controller was added or removed (bits 0x300),
    // `IVP_Simulation_Unit_Rebalance_And_Clear_Topology_Flag` rebuilds the
    // controller buckets and clears the bits, asleep or not
    // (`ivp_simulation_unit.cpp:861`, `:693`). The buckets are the world's
    // controller order, which needs no rebuild here.
    *unit_state &= !UNIT_TOPOLOGY_CHANGED;
    slept
}

/// `revive_pass` and `mark_active_and_sync_objects` for a frozen core
/// taken off the wake queue at the start of a PSI.
pub fn revive_core(core: &mut Core, unit_state: &mut u32, clock: &PsiClock) {
    core.movement_state = (core.movement_state & 0xFFFF_FF01) | 1;
    core.update_time_of_last_motion(clock.current_time);
    core.time_of_last_psi = clock.current_time;
    let (_, inv) = clock.time_to_next_psi();
    core.synchronize_simulation_time_snapshot(inv);
    *unit_state = (*unit_state & 0xFFFF_FF01) | UNIT_ACTIVE;
}

/// The environment field at +0x88 `classify_motion_state` compares the
/// idle time against (0.3).
pub const MOTION_IDLE_TIME: f32 = 0.3;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unit_psi_end_clears_the_topology_bits() {
        let mut core = Core::default();
        for motion_check in [false, true] {
            let mut unit = UNIT_ACTIVE | 0x100;
            unit_psi_end(
                &mut core,
                &mut unit,
                IvpTime::new(0.0, 0.0),
                motion_check,
                MOTION_IDLE_TIME,
            );
            assert_eq!(
                unit & UNIT_TOPOLOGY_CHANGED,
                0,
                "motion check {motion_check}"
            );
        }
    }

    #[test]
    fn clock_runs_one_psi_per_step() {
        let mut clock = PsiClock::new(initial_psi_time(16.0));
        assert_eq!(clock.delta_time, 0.0625);
        let mut starts = Vec::new();
        clock.simulate_dtime(0.05, |c| starts.push((c.current_time, c.delta_time)));
        clock.simulate_dtime(0.5, |c| starts.push((c.current_time, c.delta_time)));
        clock.simulate_dtime(0.0, |c| starts.push((c.current_time, c.delta_time)));
        clock.simulate_dtime(f32::NAN, |c| starts.push((c.current_time, c.delta_time)));
        assert_eq!(starts[0], (IvpTime::new(0.0, 0.0), 0.05));
        assert_eq!(starts[1], (IvpTime::new(0.0, 0.05), 0.1));
        assert_eq!(starts[2].1, 0.005);
        assert_eq!(starts[3].1, 0.005);
        let expected = r(e(r(e(0.05f32) + e(0.1f32))) + e(0.005f32));
        assert_eq!(starts[3].0, IvpTime::new(0.0, expected));
        assert_eq!(clock.time_of_last_psi, starts[3].0);
        assert_eq!(clock.current_time, clock.next_psi_time);
    }

    #[test]
    fn clock_carries_whole_seconds() {
        let mut clock = PsiClock::new(0.0625);
        for _ in 0..12 {
            clock.simulate_dtime(0.0999, |_| {});
        }
        assert_eq!(clock.current_time.base, 1.0);
        assert!(clock.current_time.offset < 0.2);
        let (dt, inv) = clock.time_to_next_psi();
        assert_eq!((dt, inv), (clock.delta_time, clock.inv_delta_time));
    }

    #[test]
    fn random_and_movement_check() {
        let mut random = IvpRandom::default();
        assert_eq!(random.next_float(), 75.0 / 65536.0);
        let mut check = MovementCheck::default();
        let mut random = IvpRandom::default();
        let fired: Vec<bool> = (0..10).map(|_| check.tick(&mut random)).collect();
        assert_eq!(fired.iter().filter(|f| **f).count(), 1);
        assert!(fired[9]);
        assert_eq!(check.counter, 15, "75/65536 * -5 truncates to 0");
    }

    #[test]
    fn unit_bits_and_sleep() {
        let mut core = Core {
            movement_state: 1,
            ..Core::default()
        };
        let mut unit = UNIT_ACTIVE;
        core.speed_change = [2.0, 0.0, 0.0];
        unit_psi_begin(&mut core, &mut unit, IvpTime::new(0.0, 0.1));
        assert_eq!(unit & 0x400, 0x400);
        core.speed = [0.0; 3];
        unit_psi_begin(&mut core, &mut unit, IvpTime::new(1.0, 0.25));
        assert_eq!(unit & 0x1000, 0x1000);
        assert_eq!(core.time_of_last_motion, IvpTime::new(1.0, 0.25));
        assert!(!unit_psi_end(
            &mut core,
            &mut unit,
            IvpTime::new(1.0, 0.3),
            true,
            0.3
        ));
        assert_eq!(core.movement_state & 0xFF, 2);
        assert!(unit_psi_end(
            &mut core,
            &mut unit,
            IvpTime::new(9.0, 0.0),
            true,
            0.3
        ));
        assert!(core.is_frozen());
        assert_eq!(unit & 0xFF, 8);
        let mut clock = PsiClock::new(0.1);
        clock.simulate_dtime(0.1, |_| {});
        revive_core(&mut core, &mut unit, &clock);
        assert_eq!(core.movement_state & 0xFF, 1);
        assert_eq!(unit & 0xFF, 1);
        assert_eq!(core.time_of_last_psi, clock.current_time);
    }
}
