//! Timing: the performance counter, the global game time, the machine
//! speed estimate and the frame timer `Timing::Delta` (`common_timing.cpp`,
//! work package P1.5).
//!
//! The C++ reads `QueryPerformanceCounter` and `GetTickCount` and keeps the
//! frequency and the global time in process globals. Here the counters come
//! from a [`Clock`], normally an [`InstantClock`] on [`std::time::Instant`],
//! and the state is a [`Timing`] value. The arithmetic is the C++'s: `f64`
//! seconds, the period as `1 / frequency`, and the frame timer's fallback to
//! the millisecond count when the two clocks disagree by more than 50 ms.

use std::cell::Cell;
use std::time::Instant;

use crate::diag::{self, DumpFlags};
use crate::num::{trunc_to_i32_via_i64, trunc_to_i64};

const SOURCE: &str = "Common\\CTiming.cpp";

/// The clocks the C++ reads.
pub trait Clock {
    /// `QueryPerformanceFrequency`: counts per second, `None` on failure.
    fn frequency(&self) -> Option<i64>;

    /// `QueryPerformanceCounter`: `None` on failure.
    fn counter(&self) -> Option<i64>;

    /// `GetTickCount`: milliseconds, wrapping at 2^32.
    fn tick_count(&self) -> u32;
}

/// A [`Clock`] on [`Instant`]: nanoseconds since the clock was made, and
/// milliseconds for the tick count.
#[derive(Clone, Copy, Debug)]
pub struct InstantClock {
    origin: Instant,
}

impl InstantClock {
    /// A clock starting now.
    #[must_use]
    pub fn new() -> Self {
        Self {
            origin: Instant::now(),
        }
    }
}

impl Default for InstantClock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock for InstantClock {
    fn frequency(&self) -> Option<i64> {
        Some(1_000_000_000)
    }

    fn counter(&self) -> Option<i64> {
        Some(i64::try_from(self.origin.elapsed().as_nanos()).unwrap_or(i64::MAX))
    }

    fn tick_count(&self) -> u32 {
        let millis = self.origin.elapsed().as_millis();
        (millis & u128::from(u32::MAX)) as u32
    }
}

/// A [`Clock`] whose readings are set by hand, for tests and replays.
#[derive(Clone, Debug)]
pub struct ManualClock {
    frequency: Option<i64>,
    counter: Cell<Option<i64>>,
    tick_count: Cell<u32>,
}

impl ManualClock {
    /// A clock with the given frequency, reading zero.
    #[must_use]
    pub fn new(frequency: Option<i64>) -> Self {
        Self {
            frequency,
            counter: Cell::new(Some(0)),
            tick_count: Cell::new(0),
        }
    }

    /// Sets the performance counter; `None` makes reads fail.
    pub fn set_counter(&self, counter: Option<i64>) {
        self.counter.set(counter);
    }

    /// Sets the millisecond count.
    pub fn set_tick_count(&self, tick_count: u32) {
        self.tick_count.set(tick_count);
    }

    /// Advances both readings by `counts` and `millis`.
    pub fn advance(&self, counts: i64, millis: u32) {
        self.counter
            .set(self.counter.get().map(|c| c.wrapping_add(counts)));
        self.tick_count
            .set(self.tick_count.get().wrapping_add(millis));
    }
}

impl Clock for ManualClock {
    fn frequency(&self) -> Option<i64> {
        self.frequency
    }

    fn counter(&self) -> Option<i64> {
        self.counter.get()
    }

    fn tick_count(&self) -> u32 {
        self.tick_count.get()
    }
}

impl<C: Clock + ?Sized> Clock for &C {
    fn frequency(&self) -> Option<i64> {
        (**self).frequency()
    }

    fn counter(&self) -> Option<i64> {
        (**self).counter()
    }

    fn tick_count(&self) -> u32 {
        (**self).tick_count()
    }
}

/// The state behind the C++ `Timing` namespace.
#[derive(Clone, Debug)]
pub struct Timing<C: Clock> {
    clock: C,
    frequency: f64,
    period: f64,
    global: f64,
    machine_speed_mhz: i32,
}

impl<C: Clock> Timing<C> {
    /// Uninitialised timing on `clock`; [`init`](Self::init) reads the
    /// frequency.
    #[must_use]
    pub fn new(clock: C) -> Self {
        Self {
            clock,
            frequency: 0.0,
            period: 0.0,
            global: 0.0,
            machine_speed_mhz: -1,
        }
    }

    /// The clock.
    pub fn clock(&self) -> &C {
        &self.clock
    }

    /// `Timing::init`: reads the frequency once. On failure it reports an
    /// error and the period becomes `1 / 0`.
    pub fn init(&mut self) {
        if self.frequency != 0.0 {
            return;
        }
        match self.clock.frequency() {
            Some(frequency) => {
                self.frequency = frequency as f64;
                self.period = 1.0 / self.frequency;
            }
            None => {
                diag::dump(
                    DumpFlags::ERROR,
                    &format!(
                        "{SOURCE}(311) : *** ERROR: QueryPerformanceFrequence failed. GetLastError returned 0"
                    ),
                );
                self.frequency = 0.0;
                self.period = 1.0 / self.frequency;
            }
        }
    }

    /// The frequency as a double; zero before [`init`](Self::init).
    #[must_use]
    pub fn frequency(&self) -> f64 {
        self.frequency
    }

    /// Seconds per count; zero before [`init`](Self::init).
    #[must_use]
    pub fn period(&self) -> f64 {
        self.period
    }

    /// The counter, or zero with a reported error when it fails.
    fn counter_or_zero(&self) -> i64 {
        if let Some(counter) = self.clock.counter() {
            counter
        } else {
            diag::dump(
                DumpFlags::ERROR,
                &format!("{SOURCE}(86) : *** ERROR: QueryPerformanceCounter returned false"),
            );
            0
        }
    }

    /// `Timing::read_ticks`.
    pub fn read_ticks(&mut self) -> i64 {
        self.init();
        self.counter_or_zero()
    }

    /// `Timing::seconds`: `(double)ticks * period`.
    #[must_use]
    pub fn seconds(&self, ticks: i64) -> f64 {
        ticks as f64 * self.period
    }

    /// `Timing::read`: the counter in seconds.
    pub fn read(&mut self) -> f64 {
        self.init();
        let ticks = self.counter_or_zero();
        ticks as f64 * self.period
    }

    /// `Timing::UpdateGlobalTime`.
    pub fn update_global_time(&mut self, dt: f64) {
        self.global += dt;
    }

    /// `Timing::GetGlobalTime`.
    #[must_use]
    pub fn global_time(&self) -> f64 {
        self.global
    }

    /// `Timing::GetMachineSpeed`: the clock speed in whole MHz, measured on
    /// the first call and cached. `measure_hz` stands for the C++'s
    /// `TestClockSpeed` (see [`clock_speed_hz`]); the counter it samples is
    /// the host's business.
    pub fn machine_speed(&mut self, measure_hz: impl FnOnce() -> f64) -> i32 {
        if self.machine_speed_mhz < 0 {
            self.machine_speed_mhz = machine_speed_mhz(measure_hz());
        }
        self.machine_speed_mhz
    }
}

/// `GetMachineSpeed`'s conversion of Hz: tenths of a MHz through `_ftol`,
/// then `* 0.1` through `_ftol` again.
#[must_use]
pub fn machine_speed_mhz(hz: f64) -> i32 {
    let tenths = trunc_to_i32_via_i64(hz / 1_000_000.0 * 10.0);
    trunc_to_i32_via_i64(f64::from(tenths) * 0.1)
}

/// The fallback of `TestClockSpeed` when no round converges: 750 MHz.
pub const FALLBACK_CLOCK_HZ: f64 = 1_000_000.0 * 750.0;

/// `TestClockSpeed`: up to 25 rounds of six samples, each the counter
/// delta over a 20 ms sleep (`sample` takes one). The smallest and largest
/// samples are dropped; a round converges when the other four deviate from
/// their mean by less than 0.1 % of it, and the result is the mean times 50.
/// Without convergence it reports a warning and returns
/// [`FALLBACK_CLOCK_HZ`].
pub fn clock_speed_hz(mut sample: impl FnMut() -> f64) -> f64 {
    for _round in 0..25 {
        let mut samples: Vec<f64> = (0..6).map(|_| sample()).collect();
        let mut min_index = 0;
        for i in 1..samples.len() {
            if samples[i] < samples[min_index] {
                min_index = i;
            }
        }
        samples.remove(min_index);
        let mut max_index = 0;
        for i in 1..samples.len() {
            if samples[i] > samples[max_index] {
                max_index = i;
            }
        }
        samples.remove(max_index);

        let mut mean = 0.0;
        for value in &samples {
            mean += *value;
        }
        mean /= 4.0;
        let mut max_dev = 0.0;
        for value in &samples {
            let dev = (*value - mean).abs();
            if dev > max_dev {
                max_dev = dev;
            }
        }
        if max_dev / mean < 0.001 {
            return mean * 50.0;
        }
    }
    diag::dump(
        DumpFlags::WARNING,
        &format!("{SOURCE}(286) : *** WARNING: TestClockSpeed - failed!"),
    );
    FALLBACK_CLOCK_HZ
}

/// `Timing::Delta`: a frame timer on the performance counter, checked
/// against the millisecond count.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Delta {
    dt: f64,
    start_ticks: i64,
    stop_ticks: i64,
    start_tick_count: u32,
    stop_tick_count: u32,
    started: bool,
    stopped: bool,
}

impl Delta {
    /// A timer that has not started.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// A timer with the given readings, as the C++ fields hold them.
    #[must_use]
    pub fn from_readings(
        start_ticks: i64,
        stop_ticks: i64,
        start_tick_count: u32,
        stop_tick_count: u32,
        started: bool,
        stopped: bool,
    ) -> Self {
        Self {
            dt: 0.0,
            start_ticks,
            stop_ticks,
            start_tick_count,
            stop_tick_count,
            started,
            stopped,
        }
    }

    /// The last result of [`calc_dt`](Self::calc_dt).
    #[must_use]
    pub fn dt(&self) -> f64 {
        self.dt
    }

    /// Whether the timer has started.
    #[must_use]
    pub fn is_started(&self) -> bool {
        self.started
    }

    /// Whether the timer has stopped.
    #[must_use]
    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    fn capture_start<C: Clock>(&mut self, timing: &mut Timing<C>) {
        timing.init();
        self.start_ticks = timing.counter_or_zero();
        self.start_tick_count = timing.clock.tick_count();
        self.started = true;
        self.stopped = false;
    }

    fn capture_stop<C: Clock>(&mut self, timing: &mut Timing<C>) {
        timing.init();
        self.stop_ticks = timing.counter_or_zero();
        self.stop_tick_count = timing.clock.tick_count();
        self.stopped = true;
    }

    /// `Delta::init`: starts the timer unless it has started.
    pub fn init<C: Clock>(&mut self, timing: &mut Timing<C>) {
        if !self.started {
            self.capture_start(timing);
        }
    }

    /// `Delta::start`.
    pub fn start<C: Clock>(&mut self, timing: &mut Timing<C>) {
        self.capture_start(timing);
    }

    /// `Delta::stop`: starts the timer first if it has not started.
    pub fn stop<C: Clock>(&mut self, timing: &mut Timing<C>) {
        if !self.started {
            self.capture_start(timing);
        }
        self.capture_stop(timing);
    }

    /// `Delta::swap`: the stop readings become the start readings.
    pub fn swap(&mut self) {
        if !self.stopped {
            return;
        }
        self.start_ticks = self.stop_ticks;
        self.start_tick_count = self.stop_tick_count;
        self.started = true;
        self.stopped = false;
    }

    /// `Delta::calc_dt`: the counter delta in seconds; zero unless started
    /// and stopped. When it disagrees with the millisecond delta by more
    /// than 50 ms (compared as the C++ does, in wrapping 32-bit arithmetic
    /// on the truncated milliseconds), the millisecond delta is used.
    pub fn calc_dt<C: Clock>(&mut self, timing: &Timing<C>) -> f64 {
        if !self.started || !self.stopped {
            self.dt = 0.0;
            return self.dt;
        }
        let counts = self.stop_ticks.wrapping_sub(self.start_ticks);
        self.dt = counts as f64 * timing.period;

        let [a, b, c, d, ..] = trunc_to_i64(self.dt * -1000.0).to_le_bytes();
        let neg_millis = u32::from_le_bytes([a, b, c, d]);
        let tick_delta = self.stop_tick_count.wrapping_sub(self.start_tick_count);
        let mut abs_diff = i32::from_ne_bytes(
            0u32.wrapping_sub(neg_millis)
                .wrapping_sub(tick_delta)
                .to_ne_bytes(),
        );
        if abs_diff < 0 {
            abs_diff = abs_diff.wrapping_neg();
        }
        if abs_diff > 50 {
            self.dt = f64::from(tick_delta) * 0.001;
        }
        self.dt
    }

    /// `Delta::read_dt`: stops, computes the delta and swaps, so the next
    /// call measures from here.
    pub fn read_dt<C: Clock>(&mut self, timing: &mut Timing<C>) -> f64 {
        if !self.started {
            self.start(timing);
        }
        self.stop(timing);
        let elapsed = self.calc_dt(timing);
        self.swap();
        elapsed
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delta_measures_counter_seconds() {
        let clock = ManualClock::new(Some(10_000_000));
        let mut timing = Timing::new(&clock);
        let mut delta = Delta::new();
        delta.start(&mut timing);
        let period = 1.0 / 10_000_000.0;
        clock.advance(250_000, 25);
        assert_eq!(delta.read_dt(&mut timing), 250_000.0 * period);
        clock.advance(1_000_000, 100);
        assert_eq!(delta.read_dt(&mut timing), 1_000_000.0 * period);
        assert_eq!(timing.read(), 1_250_000.0 * period);
    }

    #[test]
    fn delta_falls_back_to_the_tick_count() {
        let clock = ManualClock::new(Some(1000));
        let mut timing = Timing::new(&clock);
        let mut delta = Delta::new();
        delta.start(&mut timing);
        clock.advance(1000, 900);
        delta.stop(&mut timing);
        assert_eq!(delta.calc_dt(&timing), 900.0 * 0.001);
        clock.advance(0, 51);
        delta.swap();
        clock.advance(51, 0);
        delta.stop(&mut timing);
        assert_eq!(delta.calc_dt(&timing), 51.0 * (1.0 / 1000.0));
    }

    #[test]
    fn delta_is_zero_until_stopped() {
        let clock = ManualClock::new(Some(1000));
        let timing = Timing::new(&clock);
        let mut delta = Delta::from_readings(0, 10, 0, 10, true, false);
        assert_eq!(delta.calc_dt(&timing), 0.0);
    }

    #[test]
    fn failed_frequency_gives_an_infinite_period() {
        let clock = ManualClock::new(None);
        let mut timing = Timing::new(&clock);
        timing.init();
        assert!(timing.period().is_infinite());
        clock.set_counter(None);
        assert_eq!(timing.read_ticks(), 0);
    }

    #[test]
    fn global_time_accumulates() {
        let mut timing = Timing::new(InstantClock::new());
        timing.update_global_time(0.5);
        timing.update_global_time(0.25);
        assert_eq!(timing.global_time(), 0.75);
        assert!(timing.read() >= 0.0);
    }

    #[test]
    fn machine_speed_rounds_down_to_whole_mhz() {
        assert_eq!(machine_speed_mhz(2_394_567_890.0), 2394);
        assert_eq!(machine_speed_mhz(FALLBACK_CLOCK_HZ), 750);
        let mut timing = Timing::new(InstantClock::new());
        assert_eq!(timing.machine_speed(|| 1_000_000_000.0), 1000);
        assert_eq!(timing.machine_speed(|| 0.0), 1000);
    }

    #[test]
    fn clock_speed_drops_the_extremes() {
        let values = [
            20_000_000.0,
            1.0,
            20_000_001.0,
            1e9,
            20_000_002.0,
            20_000_003.0,
        ];
        let mut next = values.iter().copied().cycle();
        let hz = clock_speed_hz(|| next.next().unwrap_or(0.0));
        let mean = (((20_000_000.0 + 20_000_001.0) + 20_000_002.0) + 20_000_003.0) / 4.0;
        assert_eq!(hz, mean * 50.0);
        assert_eq!(clock_speed_hz(|| 0.0), FALLBACK_CLOCK_HZ);
    }
}
