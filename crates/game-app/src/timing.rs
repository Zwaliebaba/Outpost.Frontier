//! The client's frame clock (`Client/client_timing.cpp`; work package
//! P4.1): the frame-rate cap, the measured and capped frame times, the
//! frame-rate window, the pause counters and the network-stall flag.
//!
//! The C++ keeps these in process globals over a static `Timing::Delta`;
//! here they are one [`ClientTiming`] over `neuron-core::time`. The
//! millisecond stamp comes from the clock's tick count, where the C++ read
//! `timeGetTime` (winmm timers are not used, D271). The oracle's fixed
//! 30 fps pacing is instrumentation and is not ported.

use neuron_core::time::{Clock, Delta, Timing};

/// The frame-rate cap the client starts with (`G_FpsCap`).
pub const DEFAULT_FPS_CAP: f32 = 200.0;

/// The longest frame time the game sees: `1 / kMaxFps` with `kMaxFps`
/// 1.0, one second.
const MAX_FPS: f32 = 1.0;

/// The frame-rate window, in seconds.
const FPS_WINDOW_SECONDS: f64 = 2.0;

/// How long without a server message makes the network stalled, in ms.
const STALL_MS: u32 = 1000;

/// What a change of the server-tick pause asks of the sounds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SoundPause {
    /// `PauseAllClientSounds`.
    PauseAll,
    /// `ResumePausedClientSounds`.
    ResumePaused,
}

/// The client's frame clock.
pub struct ClientTiming<C: Clock> {
    timing: Timing<C>,
    delta: Delta,
    elapsed: f64,
    dt_capped: f64,
    dt_raw: f64,
    average_frame_time: f64,
    fps_window: f64,
    frame_count: u32,
    fps_window_frames: u32,
    last_tick_ms: u32,
    network_stalled: bool,
    pause_depth: i32,
    server_tick_pause_depth: i32,
    accumulated: f64,
    fps_cap: f32,
}

impl<C: Clock> ClientTiming<C> {
    /// The clock as the C++'s globals start: the frame-rate window at 10 s
    /// (so the first frame sets the average), the stamp read now.
    pub fn new(clock: C) -> Self {
        let mut timing = Timing::new(clock);
        timing.init();
        let last_tick_ms = timing.clock().tick_count();
        ClientTiming {
            timing,
            delta: Delta::new(),
            elapsed: 0.0,
            dt_capped: 0.0,
            dt_raw: 0.0,
            average_frame_time: 0.0,
            fps_window: 10.0,
            frame_count: 0,
            fps_window_frames: 0,
            last_tick_ms,
            network_stalled: false,
            pause_depth: 0,
            server_tick_pause_depth: 0,
            accumulated: 0.0,
            fps_cap: DEFAULT_FPS_CAP,
        }
    }

    #[must_use]
    pub fn clock(&self) -> &C {
        self.timing.clock()
    }

    /// `TickFrameTimer`: waits, calling `sleep` (one millisecond in the
    /// C++), until a frame of at least `1 / cap` seconds has passed, then
    /// updates the frame times. In multiplayer the network counts as
    /// stalled when the last server message (`last_message_ms`, on the
    /// same millisecond clock) is more than a second old.
    pub fn tick(&mut self, single_player: bool, last_message_ms: u32, mut sleep: impl FnMut()) {
        let min_frame = 1.0 / f64::from(self.fps_cap);
        let dt = loop {
            self.delta.stop(&mut self.timing);
            let dt = self.delta.calc_dt(&self.timing);
            if dt >= min_frame {
                break dt;
            }
            sleep();
        };
        self.delta.swap();
        self.dt_raw = dt;
        let max_frame = 1.0 / f64::from(MAX_FPS);
        self.dt_capped = if dt < max_frame { dt } else { max_frame };
        self.elapsed += self.dt_capped;
        self.frame_count = self.frame_count.wrapping_add(1);
        self.fps_window_frames = self.fps_window_frames.wrapping_add(1);
        self.fps_window += self.dt_capped;
        if self.fps_window > FPS_WINDOW_SECONDS {
            self.average_frame_time = self.fps_window / f64::from(self.fps_window_frames);
            self.fps_window_frames = 0;
            self.fps_window = 0.0;
        }
        self.last_tick_ms = self.timing.clock().tick_count();
        self.network_stalled =
            !single_player && self.last_tick_ms.wrapping_sub(last_message_ms) > STALL_MS;
    }

    /// `UpdateClientTiming`: a tick, then the client's own clock advances
    /// by the capped frame time in single player, the measured one in
    /// multiplayer (both zero while paused).
    pub fn update(&mut self, single_player: bool, last_message_ms: u32, sleep: impl FnMut()) {
        self.tick(single_player, last_message_ms, sleep);
        self.accumulated += if single_player {
            self.frame_time_capped()
        } else {
            self.frame_time_raw()
        };
    }

    /// `GetClientAccumulatedTime`.
    #[must_use]
    pub fn accumulated(&self) -> f64 {
        self.accumulated
    }

    /// `SeedClientAccumulatedTime`.
    pub fn seed_accumulated(&mut self, seconds: f64) {
        self.accumulated = seconds;
    }

    /// `GetClientFrameDeltaSeconds`: the capped time in single player, the
    /// measured one in multiplayer, as a `float`.
    #[must_use]
    pub fn frame_delta(&self, single_player: bool) -> f32 {
        (if single_player {
            self.frame_time_capped()
        } else {
            self.frame_time_raw()
        }) as f32
    }

    /// `G_PauseDepth != 0`: the frame times read zero.
    #[must_use]
    pub fn paused(&self) -> bool {
        self.pause_depth != 0
    }

    /// `GetClientFrameTimeCapped`: zero while paused.
    #[must_use]
    pub fn frame_time_capped(&self) -> f64 {
        if self.pause_depth != 0 {
            0.0
        } else {
            self.dt_capped
        }
    }

    /// `GetClientFrameTimeRaw`: zero while paused.
    #[must_use]
    pub fn frame_time_raw(&self) -> f64 {
        if self.pause_depth != 0 {
            0.0
        } else {
            self.dt_raw
        }
    }

    /// `GetClientElapsedTime`: the capped times summed, pauses or not.
    #[must_use]
    pub fn elapsed(&self) -> f64 {
        self.elapsed
    }

    /// The seconds per frame over the last window.
    #[must_use]
    pub fn average_frame_time(&self) -> f64 {
        self.average_frame_time
    }

    /// `GetClientFrameCount`.
    #[must_use]
    pub fn frame_count(&self) -> u32 {
        self.frame_count
    }

    /// `GetClientFrameTimestampMs`.
    #[must_use]
    pub fn frame_timestamp_ms(&self) -> u32 {
        self.last_tick_ms
    }

    /// Whether the last tick found the network stalled.
    #[must_use]
    pub fn network_stalled(&self) -> bool {
        self.network_stalled
    }

    /// `PushClientTimingPause`.
    pub fn push_pause(&mut self) {
        self.pause_depth += 1;
    }

    /// `PopClientTimingPause`.
    pub fn pop_pause(&mut self) {
        self.pause_depth -= 1;
    }

    /// `SetClientServerTickPause`: counts pauses of the local server's tick;
    /// in single player the first pause pauses the sounds and the last
    /// resume resumes them.
    pub fn set_server_tick_pause(
        &mut self,
        pause: bool,
        single_player: bool,
    ) -> Option<SoundPause> {
        if pause {
            let first = self.server_tick_pause_depth == 0;
            self.server_tick_pause_depth += 1;
            return (first && single_player).then_some(SoundPause::PauseAll);
        }
        self.server_tick_pause_depth -= 1;
        (self.server_tick_pause_depth == 0 && single_player).then_some(SoundPause::ResumePaused)
    }

    /// `IsClientServerTickPaused`.
    #[must_use]
    pub fn server_tick_paused(&self) -> bool {
        self.server_tick_pause_depth > 0
    }

    /// `ShouldSkipLocalServerTick`: only in single player.
    #[must_use]
    pub fn skip_local_server_tick(&self, single_player: bool) -> bool {
        single_player && self.server_tick_pause_depth > 0
    }

    /// `SetClientFpsCap`.
    pub fn set_fps_cap(&mut self, fps: f32) {
        self.fps_cap = fps;
    }

    /// `Timing::UpdateGlobalTime(dt)` (`TickClientFrame`'s first step).
    pub fn update_global_time(&mut self, dt: f64) {
        self.timing.update_global_time(dt);
    }

    /// `Timing::GetGlobalTime`.
    #[must_use]
    pub fn global_time(&self) -> f64 {
        self.timing.global_time()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::time::ManualClock;

    use std::rc::Rc;

    /// A 1 MHz manual clock the test and the timing share.
    #[derive(Clone)]
    struct Shared(Rc<ManualClock>);

    impl Clock for Shared {
        fn frequency(&self) -> Option<i64> {
            self.0.frequency()
        }
        fn counter(&self) -> Option<i64> {
            self.0.counter()
        }
        fn tick_count(&self) -> u32 {
            self.0.tick_count()
        }
    }

    fn clock() -> Shared {
        let c = ManualClock::new(Some(1_000_000));
        c.set_counter(Some(0));
        Shared(Rc::new(c))
    }

    #[test]
    fn frames_wait_for_the_cap_and_cap_at_a_second() {
        let c = clock();
        let mut t = ClientTiming::new(c.clone());
        t.set_fps_cap(100.0);
        // The first tick starts the timer and sleeps in 1 ms steps until
        // 10 ms have passed.
        let mut sleeps = 0;
        t.update(true, 0, || {
            sleeps += 1;
            c.0.advance(1000, 1);
        });
        assert_eq!(sleeps, 10);
        assert!((t.frame_time_capped() - 0.010).abs() < 1e-12);
        assert_eq!(t.frame_count(), 1);
        // The window started at 10 s, so the first frame sets the average.
        assert!((t.average_frame_time() - 10.010).abs() < 1e-9);
        assert!((t.accumulated() - 0.010).abs() < 1e-12);
        // A 3 s frame is capped at 1 s; the raw time keeps 3 s.
        let stamp = t.frame_timestamp_ms();
        c.0.advance(3_000_000, 3000);
        t.update(false, stamp, || panic!("no wait"));
        assert!((t.frame_time_raw() - 3.0).abs() < 1e-9);
        assert_eq!(t.frame_time_capped(), 1.0);
        assert!((t.elapsed() - 1.010).abs() < 1e-9);
        // Multiplayer accumulates the measured time.
        assert!((t.accumulated() - 3.010).abs() < 1e-9);
        // The last message is 3 s old: stalled in multiplayer.
        assert!(t.network_stalled());
        c.0.advance(10_000, 10);
        t.update(true, stamp, || {});
        assert!(!t.network_stalled());
    }

    #[test]
    fn pauses_zero_the_frame_times_and_count_server_pauses() {
        let mut t = ClientTiming::new(clock());
        t.push_pause();
        assert_eq!(t.frame_time_capped(), 0.0);
        t.pop_pause();
        assert_eq!(
            t.set_server_tick_pause(true, true),
            Some(SoundPause::PauseAll)
        );
        assert_eq!(t.set_server_tick_pause(true, true), None);
        assert!(t.skip_local_server_tick(true));
        assert!(!t.skip_local_server_tick(false));
        assert_eq!(t.set_server_tick_pause(false, true), None);
        assert_eq!(
            t.set_server_tick_pause(false, true),
            Some(SoundPause::ResumePaused)
        );
        assert!(!t.server_tick_paused());
        assert_eq!(t.set_server_tick_pause(true, false), None);
    }
}
