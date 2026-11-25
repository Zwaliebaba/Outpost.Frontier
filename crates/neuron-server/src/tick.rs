//! The server frame (`IServerImpl::ElapseTime`, `Server/IServerImpl.cpp:295`)
//! and the server clock.
//!
//! One frame runs, in order: the clock gains the wall-clock time since the
//! last frame (not `dt`); the systems, the phantom pump and the dying ships
//! (`Server_PhysicsUpdate`, P3.3); the subsystems (`BaseDB::update`); the
//! controller heartbeat walk with its teardown; the timers.

use crate::controller::{self, BusHost};

/// The server clock: the base a loaded game or world file sets plus the
/// wall-clock time the frames have added (`g_ElapsedTimeAccum` and
/// `GServerTimeBase`, two doubles).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ServerClock {
    /// `GServerTimeBase`.
    pub base: f64,
    /// `g_ElapsedTimeAccum`: never reset.
    pub elapsed: f64,
}

impl ServerClock {
    /// The game time (`base + elapsed`).
    #[must_use]
    pub fn now(&self) -> f64 {
        self.base + self.elapsed
    }

    /// One frame's wall-clock delta (`Timing::Delta::read_dt`), negative
    /// values taken as 0.
    pub fn advance(&mut self, real_dt: f64) {
        self.elapsed += real_dt.max(0.0);
    }
}

/// The game side of a frame.
pub trait FrameHost: BusHost {
    /// The server clock.
    fn clock(&mut self) -> &mut ServerClock;
    /// `Server_PhysicsUpdate(dt)`: the systems, phantoms and dying ships.
    fn physics_update(&mut self, dt: f32);
    /// `Server_SubsystemUpdate(dt)`: `BaseDB::update`.
    fn subsystem_update(&mut self, dt: f32);
}

/// `IServerImpl::ElapseTime(dt)` with the frame's wall-clock delta.
pub fn elapse_time<H: FrameHost>(host: &mut H, dt: f32, real_dt: f64) {
    host.clock().advance(real_dt);
    host.physics_update(dt);
    host.subsystem_update(dt);
    controller::tick(host, dt);
    controller::fire_timers(host, dt);
}

/// The dedicated server's frame period (`GServerFramePeriod`, the float
/// nearest 1/30, which nothing writes; `OutpostServer.cpp:61`).
pub const FRAME_PERIOD: f32 = 0.033_333_335;

/// The server thread's pacing (`ServerThreadProc`,
/// `OutpostServer/OutpostServer.cpp:370-403`): at the top of each frame
/// the time since the previous frame's reading is the frame's `dt`; when
/// it is below the period the thread sleeps the rest, cut to whole
/// milliseconds; then the reading taken *before* the sleep becomes the
/// previous one. So a frame that slept is followed by one whose `dt`
/// includes that sleep, and `dt` alternates between the work time and
/// about a period.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FramePacer<T> {
    last: T,
}

/// A frame's start as [`FramePacer::begin`] answers it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FrameStart {
    /// The frame's `dt`, in seconds (`Timing::seconds(DeltaTicks)`).
    pub elapsed: f64,
    /// How long to sleep before the frame runs.
    pub sleep: std::time::Duration,
}

impl<T> FramePacer<T>
where
    T: Copy + core::ops::Sub<T, Output = std::time::Duration>,
{
    /// A pacer whose first reading is `now` (`LastTicks`, read before the
    /// loop).
    pub const fn new(now: T) -> Self {
        Self { last: now }
    }

    /// The frame that starts at `now`.
    pub fn begin(&mut self, now: T) -> FrameStart {
        let elapsed = (now - self.last).as_secs_f64();
        let period = f64::from(FRAME_PERIOD);
        let sleep = if period > elapsed {
            let millis = neuron_core::num::trunc_to_i32((period - elapsed) * 1000.0);
            std::time::Duration::from_millis(u64::try_from(millis).unwrap_or(0))
        } else {
            std::time::Duration::ZERO
        };
        self.last = now;
        FrameStart { elapsed, sleep }
    }
}

/// The slots of the load history (`COutpostServerDoc::LoadHistorySize`).
pub const LOAD_HISTORY: usize = 64;

/// The frame load the status shows (`COutpostServerDoc::SetLoad`,
/// `GetAverageLoad`): each frame's milliseconds in a ring of 64 slots,
/// starting at zero, and their integer mean.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoadHistory {
    slots: [i32; LOAD_HISTORY],
    next: usize,
    last: i32,
}

impl Default for LoadHistory {
    fn default() -> Self {
        Self {
            slots: [0; LOAD_HISTORY],
            next: 0,
            last: 0,
        }
    }
}

impl LoadHistory {
    /// `SetLoad`.
    pub fn push(&mut self, milliseconds: i32) {
        self.last = milliseconds;
        self.slots[self.next] = milliseconds;
        self.next = (self.next + 1) % LOAD_HISTORY;
    }

    /// The last frame's load.
    #[must_use]
    pub const fn last(&self) -> i32 {
        self.last
    }

    /// `GetAverageLoad`: the sum over every slot divided by 64.
    #[must_use]
    pub fn average(&self) -> i32 {
        self.slots.iter().sum::<i32>() / LOAD_HISTORY as i32
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::controller::{ControllerBus, ControllerId, Priority};

    #[derive(Default)]
    struct Host {
        bus: ControllerBus<()>,
        clock: ServerClock,
        log: Vec<String>,
        next: u32,
    }

    impl BusHost for Host {
        type Kind = ();
        type Message<'m> = &'static str;
        type CreateParams = ();
        fn bus(&mut self) -> &mut ControllerBus<()> {
            &mut self.bus
        }
        fn deliver(&mut self, _id: ControllerId, _kind: (), message: &mut &'static str) {
            self.log.push((*message).to_owned());
        }
        fn instantiate(&mut self, _: ControllerId, _: &[u8], _: &[u8], _: &()) -> Option<()> {
            Some(())
        }
        fn destroy_instance(&mut self, _: ControllerId, _: ()) {
            self.log.push("destroyed".into());
        }
        fn allocate_id(&mut self) -> u32 {
            self.next += 1;
            self.next
        }
        fn rand(&mut self) -> i32 {
            0
        }
        fn heartbeat_message(_: f32) -> &'static str {
            "heartbeat"
        }
        fn timer_expired_message(_: u32, _: u32) -> &'static str {
            "timer"
        }
        fn controller_destroyed_message(_: ControllerId) -> &'static str {
            "controller destroyed"
        }
    }

    impl FrameHost for Host {
        fn clock(&mut self) -> &mut ServerClock {
            &mut self.clock
        }
        fn physics_update(&mut self, _dt: f32) {
            self.log.push("physics".into());
        }
        fn subsystem_update(&mut self, _dt: f32) {
            self.log.push("subsystems".into());
        }
    }

    #[test]
    fn a_frame_runs_in_the_reference_order() {
        let mut h = Host::default();
        let id = controller::create(&mut h, b"", b"", &(), Priority::Normal);
        controller::set_heartbeat(&mut h, id, 0.5).unwrap();
        h.bus.set_timer(id, 1, 2, 0.1).unwrap();
        controller::destroy(&mut h, id);
        h.log.clear();
        elapse_time(&mut h, 1.0, -3.0);
        assert_eq!(h.log, ["physics", "subsystems", "destroyed"]);
        assert_eq!(h.clock.elapsed, 0.0);
        h.clock.base = 10.0;
        elapse_time(&mut h, 1.0, 0.25);
        assert_eq!(h.clock.now(), 10.25);
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    struct Ms(u64);

    impl core::ops::Sub for Ms {
        type Output = std::time::Duration;
        fn sub(self, other: Self) -> std::time::Duration {
            std::time::Duration::from_millis(self.0 - other.0)
        }
    }

    #[test]
    fn the_pacer_sleeps_the_rest_of_the_period_from_the_last_reading() {
        let mut pacer = FramePacer::new(Ms(0));
        // The first frame starts at once and sleeps nearly a whole period.
        let first = pacer.begin(Ms(1));
        assert_eq!(first.elapsed, 0.001);
        assert_eq!(first.sleep.as_millis(), 32);
        // The next reading comes after the sleep and 2 ms of work: its dt
        // includes the sleep, and it does not sleep.
        let second = pacer.begin(Ms(35));
        assert_eq!(second.elapsed, 0.034);
        assert_eq!(second.sleep, std::time::Duration::ZERO);
        // The one after is only the work time again.
        let third = pacer.begin(Ms(37));
        assert_eq!(third.elapsed, 0.002);
        assert_eq!(third.sleep.as_millis(), 31);
    }

    #[test]
    fn the_load_is_the_mean_of_64_slots() {
        let mut load = LoadHistory::default();
        load.push(64);
        assert_eq!(load.average(), 1);
        assert_eq!(load.last(), 64);
        for _ in 0..LOAD_HISTORY {
            load.push(10);
        }
        assert_eq!(load.average(), 10);
    }
}
